#include "teams/TeamCompletionPolicy.h"

#include "teams/TeamRoutingEvidence.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QRegularExpression>
#include <QUrl>
#include <algorithm>

namespace ketplus::daemon::teams::completion {
namespace {
bool fullHead(const QString& head) {
    static const QRegularExpression expression(QStringLiteral("\\A[0-9a-f]{40}\\z"));
    return expression.match(head).hasMatch();
}
bool sourceKnown(const QString& source) {
    return source == "leader_policy" || source == "independent_review" ||
           source == "leader_targeted_verification";
}
} // namespace

std::optional<TaskAcceptance> fromJson(const QJsonValue& value) {
    const auto object = value.toObject();
    TaskAcceptance result{
        object.value("head").toString(),   object.value("context").toString(),
        object.value("source").toString(), object.value("actor").toString(),
        object.value("at").toString(),     object.value("repository_id").toString()};
    if (object.value("version").toInt() != 1 || !fullHead(result.head) ||
        !risk::evidenceId(result.context) || !sourceKnown(result.source) ||
        result.actor.isEmpty() || result.actor.size() > 200 ||
        !QDateTime::fromString(result.at, Qt::ISODateWithMs).isValid() ||
        (!result.repositoryId.isEmpty() && !risk::evidenceId(result.repositoryId)))
        return std::nullopt;
    return result;
}

QJsonObject toJson(const TaskAcceptance& acceptance) {
    return {{"version", 1},
            {"head", acceptance.head},
            {"context", acceptance.context},
            {"source", acceptance.source},
            {"actor", acceptance.actor},
            {"at", acceptance.at},
            {"repository_id", acceptance.repositoryId}};
}

QString repositoryId(QString url) {
    if (url.startsWith("git@") && !url.contains("://"))
        url = QStringLiteral("ssh://") + url.replace(u':', u'/');
    const QUrl parsed(url);
    const auto pieces = parsed.path().split(u'/', Qt::SkipEmptyParts);
    if (!parsed.isValid() || parsed.host().isEmpty() ||
        (parsed.scheme() != "https" && parsed.scheme() != "ssh") || !parsed.query().isEmpty() ||
        !parsed.fragment().isEmpty() ||
        (parsed.port(-1) != -1 && parsed.port() != (parsed.scheme() == "ssh" ? 22 : 443)) ||
        (pieces.size() != 2 && !(pieces.size() == 4 && pieces[2] == "pull")))
        return {};
    QString repository = pieces[1];
    if (repository.endsWith(".git"))
        repository.chop(4);
    static const QRegularExpression segment(QStringLiteral("\\A[A-Za-z0-9_.-]+\\z"));
    if (!segment.match(pieces[0]).hasMatch() || !segment.match(repository).hasMatch())
        return {};
    const QByteArray canonical =
        (parsed.host().toLower() + u'/' + pieces[0].toLower() + u'/' + repository.toLower())
            .toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

QString context(const Team& team, const Task& task) {
    // Deliberately separate from receipt lifetime. Only daemon-generated audit,
    // transport revisions, merge UI bookkeeping and merge decisions are normalized;
    // the accepted delta, findings, route, requirements and policy remain bound.
    Task accepted = task;
    accepted.acceptance.reset();
    accepted.state = state::ready;
    accepted.changedRevision = 0;
    accepted.history.clear();
    accepted.mergeOffered = false;
    accepted.leaderMayMerge = false;
    // A reviewer assesses the requirement itself, not the submitter's checkbox.
    // Fast-pass checkboxes are independently mandatory in valid()/decideRoute().
    for (auto& criterion : accepted.criteria)
        criterion.met = true;
    Team policy = team;
    for (const QString& id : task.dependsOn)
        if (Task* dependency = policy.task(id)) {
            dependency->state = state::todo;
            dependency->mergeOffered = false;
            dependency->leaderMayMerge = false;
        }
    policy.decisions.removeIf(
        [&](const Decision& d) { return d.kind == "merge" && d.taskId == task.id; });
    return RoutingEvidence::semanticRevision(policy, accepted, task.headCommit);
}

void seal(const Team& team, Task& task, const QString& source, const QString& actor,
          const QString& repository) {
    task.acceptance = TaskAcceptance{task.headCommit,
                                     context(team, task),
                                     source,
                                     actor,
                                     QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
                                     repository};
}

bool valid(const Team& team, const Task& task) {
    if (!task.acceptance || task.state != state::ready || !fullHead(task.headCommit) ||
        task.headCommit != task.acceptance->head || !task.reviewBlock.isEmpty() ||
        !sourceKnown(task.acceptance->source) ||
        (task.reviewFindings && review_policy::blocks(*task.reviewFindings)))
        return false;
    const Member* actor = team.member(task.acceptance->actor);
    if (!actor || actor->sessionId == task.owner ||
        (task.acceptance->source == "independent_review" ? actor->role != role::reviewer
                                                         : actor->role != role::leader))
        return false;
    if (task.acceptance->source != "independent_review" &&
        (!std::all_of(task.criteria.cbegin(), task.criteria.cend(),
                      [](const Criterion& c) { return c.met; }) ||
         !std::all_of(task.dependsOn.cbegin(), task.dependsOn.cend(), [&](const QString& id) {
             const Task* dependency = team.task(id);
             return dependency && dependency->state == state::done;
         })))
        return false;
    return task.acceptance->context == context(team, task);
}

bool canFinish(const Team& team, const Task& task) {
    return valid(team, task) &&
           std::all_of(task.dependsOn.cbegin(), task.dependsOn.cend(), [&](const QString& id) {
               const Task* dependency = team.task(id);
               return dependency && dependency->state == state::done;
           });
}
} // namespace ketplus::daemon::teams::completion
