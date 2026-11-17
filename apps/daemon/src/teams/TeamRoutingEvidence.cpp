#include "teams/TeamRoutingEvidence.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>

namespace ketplus::daemon::teams {
namespace {
constexpr qsizetype maximumOutput = 256 * 1024;
constexpr qsizetype maximumFile = 64 * 1024;

QString digest(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// No shell, hooks, external diff/textconv, replacement objects or inherited
// GIT_* repository overrides. Every operation reads the immutable named objects.
std::optional<QByteArray> git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const QString& key : environment.keys())
        if (key.startsWith(QStringLiteral("GIT_")))
            environment.remove(key);
    environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
    environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QProcess::nullDevice());
    environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    process.setProcessEnvironment(environment);
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("--no-replace-objects"), QStringLiteral("-c"),
                              QStringLiteral("core.fsmonitor=false"), QStringLiteral("-c"),
                              QStringLiteral("core.untrackedCache=false")} +
                      arguments);
    QElapsedTimer elapsed;
    elapsed.start();
    QByteArray output;
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(20);
        output += process.readAllStandardOutput();
        const auto errors = process.readAllStandardError();
        if (output.size() > maximumOutput || errors.size() > maximumOutput ||
            elapsed.elapsed() > 2000) {
            process.kill();
            process.waitForFinished(1000);
            return std::nullopt;
        }
    }
    output += process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
        output.size() > maximumOutput)
        return std::nullopt;
    return output;
}

// A deliberately small grammar: plain prose paragraphs, no Markdown constructs,
// directives, links, code, indentation or hard breaks. Paragraph/word identity is
// checked independently of paths. Unknown syntax is not claimed equivalent.
std::optional<QStringList> prose(const QByteArray& bytes) {
    if (bytes.isEmpty() || bytes.size() > maximumFile || !bytes.endsWith('\n'))
        return std::nullopt;
    const QString text = QString::fromUtf8(bytes);
    if (text.toUtf8() != bytes)
        return std::nullopt;
    QStringList paragraphs;
    QString paragraph;
    for (const QString& line : text.chopped(1).split(QLatin1Char('\n'))) {
        if (line.isEmpty()) {
            if (paragraph.isEmpty())
                return std::nullopt;
            paragraphs.append(paragraph);
            paragraph.clear();
            continue;
        }
        if (!line.front().isLetter() || line.endsWith(QLatin1Char(' ')))
            return std::nullopt;
        for (const QChar ch : line)
            if (!ch.isLetterOrNumber() && ch.category() != QChar::Mark_NonSpacing &&
                !QStringLiteral(" ,.;:!?()'\"-").contains(ch))
                return std::nullopt;
        if (!paragraph.isEmpty())
            paragraph += QLatin1Char(' ');
        paragraph += line.simplified();
    }
    if (!paragraph.isEmpty())
        paragraphs.append(paragraph);
    return paragraphs.isEmpty() ? std::nullopt : std::optional{paragraphs};
}

std::optional<QByteArray> blob(const QString& worktree, const QString& revision,
                               const QString& path) {
    const auto entry = git(worktree, {QStringLiteral("ls-tree"), QStringLiteral("-z"), revision,
                                      QStringLiteral("--"), path});
    if (!entry || !entry->startsWith("100644 blob ") || entry->count('\0') != 1)
        return std::nullopt;
    const QString object = revision + QLatin1Char(':') + path;
    const auto size = git(worktree, {QStringLiteral("cat-file"), QStringLiteral("-s"), object});
    bool valid = false;
    const qlonglong count = size ? size->trimmed().toLongLong(&valid) : -1;
    if (!valid || count <= 0 || count > maximumFile)
        return std::nullopt;
    return git(worktree, {QStringLiteral("cat-file"), QStringLiteral("blob"), object});
}

bool decisionsOpen(const Team& team, const Task& task) {
    for (const auto& decision : team.decisions)
        if (decision.state == QStringLiteral("open") &&
            (decision.taskId.isEmpty() || decision.taskId == task.id))
            return true;
    return false;
}
// This is a separate finding-closure recipe, selected only from the daemon's
// current review record. A submitter's severity or check assertion cannot select it.
enum class Closure { Unsupported, LineWidth, PreferenceDeferral };
Closure supportedClosure(const Team& team, const Task& task) {
    if (!task.reviewFindings || task.reviewFindings->legacy ||
        task.reviewFindings->round != task.round || task.round < 1 ||
        task.reviewFindings->revision != task.headCommit ||
        !review_policy::revision(task.reviewFindings->revision))
        return Closure::Unsupported;
    const auto* reviewer = team.member(task.reviewFindings->reviewer);
    if (!reviewer || reviewer->role != role::reviewer)
        return Closure::Unsupported;
    Closure selected = Closure::Unsupported;
    for (const auto& f : task.reviewFindings->findings) {
        if (f.status == "non_blocking" && !review_policy::blocks(f))
            continue;
        if (f.status != "open" || !review_policy::leaderVerifiable(f) || f.mandatoryRule)
            return Closure::Unsupported;
        Closure candidate = Closure::Unsupported;
        if ((f.impact == "behavior" || f.impact == "preference") && f.scope == "task" &&
            f.closeWhen == "prose_line_width_80")
            candidate = Closure::LineWidth;
        if (review_policy::deferrable(f) && f.impact == "preference" &&
            f.closeWhen == "owner_accepts_outside_preference")
            candidate = Closure::PreferenceDeferral;
        if (candidate == Closure::Unsupported ||
            (selected != Closure::Unsupported && selected != candidate))
            return Closure::Unsupported;
        selected = candidate;
    }
    return selected;
}
std::optional<QList<QByteArray>> changedFiles(const QString& directory, const QString& before,
                                              const QString& after) {
    const auto paths =
        git(directory, {"diff", "--no-ext-diff", "--no-textconv", "--no-renames", "--no-relative",
                        "--ignore-submodules=none", "--name-only", "-z", before, after, "--"});
    if (!paths || (!paths->isEmpty() && !paths->endsWith('\0')))
        return std::nullopt;
    return paths->isEmpty() ? QList<QByteArray>{} : paths->chopped(1).split('\0');
}
bool overWidth(const QByteArray& bytes) {
    for (const auto& line : QString::fromUtf8(bytes).split('\n'))
        if (line.toUcs4().size() > 80)
            return true;
    return false;
}
} // namespace

RoutingEvidence::Result RoutingEvidence::produce(const Team& team, const Task& task,
                                                 const QString& revision) {
    const auto unknown = [](const QString& reason) { return Result{std::nullopt, reason, {}}; };
    const QStringList coveredDomains{"security",          "authentication",
                                     "secrets",           "billing",
                                     "data_deletion",     "migration",
                                     "automatic_cleanup", "signing",
                                     "updates_releases",  "protocol_compatibility",
                                     "large_blast_radius"};
    // A new policy domain needs an explicit update to this proof recipe.
    if (risk::mandatoryDomains() != coveredDomains)
        return unknown(QStringLiteral("risk_coverage_version_unknown"));
    static const QRegularExpression commit(QStringLiteral("\\A[0-9a-f]{40}\\z"));
    if (!commit.match(revision).hasMatch() || !commit.match(task.baseCommit).hasMatch() ||
        task.worktree.isEmpty() || task.branch.isEmpty() || task.changedRevision < 0)
        return unknown(QStringLiteral("provenance_missing"));
    const Team scope = taskScope(team, task);
    const bool reviewedTask = task.reviewFindings.has_value();
    const Closure closure = reviewedTask ? supportedClosure(team, task) : Closure::Unsupported;
    const bool fixing = closure == Closure::LineWidth;
    const bool deferring = closure == Closure::PreferenceDeferral;
    if (!team.planApproved || task.assignmentHeld ||
        scope.integrationBranch != QStringLiteral("integration") ||
        task.state != state::inProgress || !task.reviewBlock.isEmpty() ||
        !task.outsideLane.isEmpty() || decisionsOpen(team, task))
        return unknown(QStringLiteral("review_or_approval_required"));
    if (reviewedTask) {
        if (closure == Closure::Unsupported)
            return unknown(QStringLiteral("finding_fix_unsupported"));
    } else {
        if (task.round != 0 || !task.reviewComment.isEmpty())
            return unknown(QStringLiteral("review_or_approval_required"));
        for (const auto& entry : task.history)
            if (entry.kind == QStringLiteral("verdict") ||
                entry.kind == QStringLiteral("review_accepted") ||
                entry.kind == QStringLiteral("findings_resolved"))
                return unknown(QStringLiteral("review_history"));
    }
    const auto head = git(task.worktree, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
    const auto branch = git(task.worktree, {QStringLiteral("symbolic-ref"),
                                            QStringLiteral("--short"), QStringLiteral("HEAD")});
    const auto clean =
        git(task.worktree,
            {QStringLiteral("status"), QStringLiteral("--porcelain=v1"), QStringLiteral("-z"),
             QStringLiteral("--untracked-files=all"), QStringLiteral("--ignore-submodules=none")});
    const auto ancestor =
        git(task.worktree, {QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"),
                            task.baseCommit, revision});
    if (!head || head->trimmed() != revision.toLatin1() || !branch ||
        branch->trimmed() != task.branch.toUtf8() || !clean || !clean->isEmpty() || !ancestor)
        return unknown(QStringLiteral("stale_revision"));
    const auto paths = changedFiles(task.worktree, task.baseCommit, revision);
    if (!paths || (!reviewedTask && paths->isEmpty()))
        return unknown(QStringLiteral("scope_unknown"));
    auto files = *paths;
    QByteArray proof;
    QString reviewed;
    if (reviewedTask) {
        reviewed = task.reviewFindings->revision;
        if (deferring && reviewed != revision)
            return unknown(QStringLiteral("deferral_revision_changed"));
        if (!git(task.worktree, {"merge-base", "--is-ancestor", task.baseCommit, reviewed}) ||
            !git(task.worktree, {"merge-base", "--is-ancestor", reviewed, revision}))
            return unknown(QStringLiteral("reviewed_baseline_unknown"));
        const auto initial = changedFiles(task.worktree, task.baseCommit, reviewed);
        const auto fix = changedFiles(task.worktree, reviewed, revision);
        if (!initial || !fix || (fixing && fix->isEmpty()))
            return unknown(QStringLiteral("finding_fix_scope_unknown"));
        files.append(*initial);
        files.append(*fix);
        proof += reviewed.toLatin1() + QJsonDocument(review_policy::toJson(*task.reviewFindings))
                                           .toJson(QJsonDocument::Compact);
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    if (files.isEmpty() || files.size() > 8)
        return unknown(QStringLiteral("scope_too_large"));
    bool correctedLongLine = false;
    static const QRegularExpression document(QStringLiteral("\\Adocs/[a-zA-Z0-9_./-]+\\.md\\z"));
    for (const QByteArray& file : files) {
        const QString path = QString::fromUtf8(file);
        if (path.toUtf8() != file || !document.match(path).hasMatch() || path.contains(".."))
            return unknown(QStringLiteral("unsupported_semantic_scope"));
        const auto before = blob(task.worktree, task.baseCommit, path);
        const auto after = blob(task.worktree, revision, path);
        if (!before || !after)
            return unknown(QStringLiteral("unsupported_file_mode_or_content"));
        const auto oldProse = prose(*before);
        const auto newProse = prose(*after);
        if (!oldProse || !newProse || *oldProse != *newProse)
            return unknown(QStringLiteral("semantic_coverage_unknown"));
        proof += file + '\0' + digest(*before).toLatin1() + digest(*after).toLatin1();
        if (reviewedTask) {
            const auto baseline = blob(task.worktree, reviewed, path);
            const auto baselineProse = baseline ? prose(*baseline) : std::nullopt;
            if (!baselineProse || *baselineProse != *newProse || (fixing && overWidth(*after)))
                return unknown(QStringLiteral("finding_fix_semantics_unknown"));
            correctedLongLine = correctedLongLine || overWidth(*baseline);
            proof += digest(*baseline).toLatin1();
        }
    }
    if (fixing && !correctedLongLine)
        return unknown(QStringLiteral("finding_condition_not_demonstrated"));
    if (!git(task.worktree, {QStringLiteral("diff"), QStringLiteral("--no-ext-diff"),
                             QStringLiteral("--no-textconv"), QStringLiteral("--no-relative"),
                             QStringLiteral("--check"), QStringLiteral("--ignore-submodules=none"),
                             task.baseCommit, revision, QStringLiteral("--")}))
        return unknown(QStringLiteral("check_failed"));
    // Recheck after every recipe check. Concurrent edits cannot mint a clean receipt.
    const auto finalHead =
        git(task.worktree, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
    const auto finalClean =
        git(task.worktree,
            {QStringLiteral("status"), QStringLiteral("--porcelain=v1"),
             QStringLiteral("--untracked-files=all"), QStringLiteral("--ignore-submodules=none")});
    const auto finalBranch = git(task.worktree, {"symbolic-ref", "--short", "HEAD"});
    if (!finalHead || *finalHead != *head || !finalBranch || *finalBranch != *branch ||
        !finalClean || !finalClean->isEmpty())
        return unknown(QStringLiteral("stale_revision"));

    risk::VerifiedEvidence evidence;
    evidence.teamId = team.id;
    evidence.taskId = task.id;
    evidence.taskRevision = task.changedRevision;
    evidence.revision = revision;
    const QString recipe = fixing      ? QStringLiteral("prose_line_width_fix_v1")
                           : deferring ? QStringLiteral("prose_preference_deferral_v1")
                                       : QStringLiteral("prose_reflow_v1");
    if (reviewedTask) {
        evidence.reviewedRevision = reviewed;
        evidence.localFixVerified = fixing;
    }
    evidence.id =
        digest(QUuid::createUuid().toRfc4122() + recipe.toUtf8() + QByteArray::number(team.id) +
               task.id.toUtf8() + QByteArray::number(task.changedRevision) + revision.toLatin1() +
               task.baseCommit.toLatin1() + proof);
    evidence.scopeEvidence = digest(evidence.id.toLatin1() + "scope" + proof);
    evidence.rollbackEvidence =
        digest(evidence.id.toLatin1() + "rollback" + task.baseCommit.toLatin1());
    for (const QString& domain : coveredDomains)
        evidence.domains.insert(domain, risk::Signal::Clear);
    evidence.requiredChecks = {QStringLiteral("git_clean_revision"),
                               QStringLiteral("prose_reflow_equivalence"),
                               QStringLiteral("scope_modes"), QStringLiteral("git_diff_check")};
    if (fixing)
        evidence.requiredChecks << QStringLiteral("reviewed_prose_equivalence")
                                << QStringLiteral("reviewed_prose_line_width_80");
    if (deferring)
        evidence.requiredChecks << QStringLiteral("reviewed_preference_nonblocking");
    for (const QString& name : evidence.requiredChecks)
        evidence.checks.append(
            {name, revision, digest(evidence.id.toLatin1() + name.toUtf8()), true});
    // The proof establishes no semantic change, not general safety of arbitrary docs/code.
    evidence.materialRiskKnown = evidence.lowRiskVerified = true;
    evidence.unresolvedFindings = evidence.unresolvedP0 = evidence.unresolvedP1 =
        evidence.unresolvedFailures = 0;
    // This recipe reads Git objects only. It approves no operation or unanswered decision.
    evidence.userApprovalsSatisfied = true;
    return {evidence,
            fixing      ? QStringLiteral("verified_prose_line_width_fix")
            : deferring ? QStringLiteral("verified_prose_preference_deferral")
                        : QStringLiteral("verified_prose_reflow"),
            recipe};
}

void RoutingEvidence::retain(const Team& team, const Task& task,
                             const risk::VerifiedEvidence& evidence) {
    if (receipts_.size() >= 128)
        receipts_.erase(receipts_.begin());
    const auto scope = taskScope(team, task);
    receipts_.insert(evidence.id, {evidence, task.worktree, task.branch, task.baseCommit,
                                   scope.repository, scope.integrationBranch, team.revision,
                                   QDateTime::currentDateTimeUtc().addSecs(600)});
}

std::optional<risk::VerifiedEvidence> RoutingEvidence::lookup(const Team& team, const Task& task,
                                                              const QString& id) const {
    const auto it = receipts_.constFind(id);
    const auto scope = taskScope(team, task);
    if (it == receipts_.cend() || it->expires <= QDateTime::currentDateTimeUtc() ||
        it->teamRevision != team.revision || it->evidence.teamId != team.id ||
        it->evidence.taskId != task.id || it->evidence.taskRevision != task.changedRevision ||
        it->worktree != task.worktree || it->branch != task.branch || it->base != task.baseCommit ||
        it->repository != scope.repository || it->integration != scope.integrationBranch ||
        !team.planApproved || task.assignmentHeld || decisionsOpen(team, task))
        return std::nullopt;
    // taskCommit() can name a branch tip even after a checkout switches elsewhere.
    // Independently fence the checkout as well as the coordinator's branch/head fence.
    const auto head = git(task.worktree, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
    const auto branch = git(task.worktree, {QStringLiteral("symbolic-ref"),
                                            QStringLiteral("--short"), QStringLiteral("HEAD")});
    const auto clean =
        git(task.worktree,
            {QStringLiteral("status"), QStringLiteral("--porcelain=v1"), QStringLiteral("-z"),
             QStringLiteral("--untracked-files=all"), QStringLiteral("--ignore-submodules=none")});
    if (!head || head->trimmed() != it->evidence.revision.toLatin1() || !branch ||
        branch->trimmed() != it->branch.toUtf8() || !clean || !clean->isEmpty())
        return std::nullopt;
    return it->evidence;
}
} // namespace ketplus::daemon::teams
