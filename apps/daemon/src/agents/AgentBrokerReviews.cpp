// ADR 0067: automatic review of a conversation's finished turns by its chosen reviewer.
#include "agents/AgentBroker.h"
#include "storage/LocalJournal.h"

#include <QDateTime>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

#include <functional>

namespace ketplus::daemon {
namespace {
constexpr int gitTimeoutMs = 10'000;
constexpr qsizetype userRequestBytes = 4 * 1024;
// Room for the header and the closing instructions inside the message limit.
constexpr qsizetype diffBudgetBytes = AgentBroker::maximumBodyBytes - 8 * 1024;

QString nowIso() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QString utf8Prefix(const QString& text, const qsizetype maximumBytes) {
    if (text.toUtf8().size() <= maximumBytes)
        return text;
    QString result = text.left(maximumBytes);
    while (!result.isEmpty() && result.toUtf8().size() > maximumBytes)
        result.chop(qMax<qsizetype>(1, result.size() / 16));
    return result;
}

// Runs git off the event loop; `done` receives stdout, or nothing when git failed.
void runGit(QObject* owner, const QString& directory, const QStringList& arguments,
            std::function<void(std::optional<QString>)> done) {
    auto* process = new QProcess(owner);
    process->setWorkingDirectory(directory);
    auto finish = std::make_shared<std::function<void(std::optional<QString>)>>(std::move(done));
    QObject::connect(process, &QProcess::finished, owner,
                     [process, finish](const int code, const QProcess::ExitStatus status) {
                         const bool ok = status == QProcess::NormalExit && code == 0;
                         (*finish)(ok ? std::optional<QString>(
                                            QString::fromUtf8(process->readAllStandardOutput()))
                                      : std::nullopt);
                         process->deleteLater();
                     });
    QObject::connect(process, &QProcess::errorOccurred, owner,
                     [process, finish](const QProcess::ProcessError error) {
                         if (error != QProcess::FailedToStart)
                             return;
                         (*finish)(std::nullopt);
                         process->deleteLater();
                     });
    QTimer::singleShot(gitTimeoutMs, process, [process] { process->kill(); });
    process->start(QStringLiteral("git"),
                   QStringList{QStringLiteral("--no-optional-locks")} + arguments);
}

QSet<QString> untrackedFiles(const QString& status) {
    QSet<QString> files;
    for (const QString& line : status.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        if (line.startsWith(QStringLiteral("?? ")))
            files.insert(line.mid(3));
    return files;
}

QStringList changedFiles(const QString& diff) {
    static const QRegularExpression header(QStringLiteral("^diff --git a/(.+) b/(.+)$"),
                                           QRegularExpression::MultilineOption);
    QStringList files;
    for (auto match = header.globalMatch(diff); match.hasNext();)
        files.append(match.next().captured(2));
    return files;
}

// The reviewer's last "Verdict:" line. A missing one counts as findings; rounds still bound it.
bool reportsNoFindings(const QString& text) {
    static const QRegularExpression verdict(
        QStringLiteral("^\\W*verdict\\W*:\\s*(no findings|findings)\\b"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    QString last;
    for (auto match = verdict.globalMatch(text); match.hasNext();)
        last = match.next().captured(1).toLower();
    return last == QStringLiteral("no findings");
}

QString reviewBody(const AgentConversation& working, const QString& base, const int round,
                   const int rounds, const QString& userRequest, const QString& diff,
                   const QStringList& added) {
    const QStringList files = changedFiles(diff);
    QString shown = diff;
    QStringList omitted;
    if (shown.toUtf8().size() > diffBudgetBytes) {
        shown = utf8Prefix(diff, diffBudgetBytes);
        const QStringList visible = changedFiles(shown);
        for (const QString& file : files)
            if (!visible.contains(file))
                omitted.append(file);
    }
    QStringList lines{
        QStringLiteral("Automatic review request from #%1 \"%2\" (round %3 of %4).")
            .arg(working.number, working.title)
            .arg(round)
            .arg(rounds),
        QStringLiteral("Worktree: %1").arg(working.worktree),
        QStringLiteral("Changes since: %1").arg(base),
        QString{},
        QStringLiteral("The user's request:"),
        userRequest.isEmpty() ? QStringLiteral("(not recorded)")
                              : utf8Prefix(userRequest, userRequestBytes),
        QString{},
        QStringLiteral("Changed files: %1")
            .arg(files.isEmpty() ? QStringLiteral("none") : files.join(QStringLiteral(", ")))};
    if (!added.isEmpty())
        lines << QStringLiteral("New untracked files (read them in the worktree): %1")
                     .arg(added.join(QStringLiteral(", ")));
    if (!shown.isEmpty())
        lines << QString{} << QStringLiteral("```diff") << shown.trimmed() << QStringLiteral("```");
    if (!omitted.isEmpty())
        lines << QStringLiteral("The diff was shortened; read these files in the worktree: %1")
                     .arg(omitted.join(QStringLiteral(", ")));
    lines << QString{}
          << QStringLiteral("Review these changes read-only. For each finding give the file, line, "
                            "severity and a one-line summary, or say there are none. End with "
                            "exactly one line: \"Verdict: findings\" or \"Verdict: no findings\".");
    return lines.join(QLatin1Char('\n'));
}

} // namespace

void AgentBroker::snapshotWorktree(const QString& sessionId, const QString& worktree,
                                   std::function<void(ReviewSnapshot)> done) {
    QPointer<AgentBroker> self(this);
    // `stash create` records uncommitted changes without touching the worktree.
    runGit(this, worktree, {QStringLiteral("stash"), QStringLiteral("create")},
           [self, worktree, done](std::optional<QString> stash) {
               if (!self)
                   return;
               auto withBase = [self, worktree, done](const QString& base) {
                   runGit(self, worktree,
                          {QStringLiteral("status"), QStringLiteral("--porcelain=v1"),
                           QStringLiteral("--untracked-files=all")},
                          [done, base](std::optional<QString> status) {
                              done({base, untrackedFiles(status.value_or(QString{}))});
                          });
               };
               if (stash && !stash->trimmed().isEmpty()) {
                   withBase(stash->trimmed());
                   return;
               }
               runGit(self, worktree, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")},
                      [withBase](std::optional<QString> head) {
                          withBase(head ? head->trimmed() : QString{});
                      });
           });
    Q_UNUSED(sessionId);
}

void AgentBroker::noteReviewerAssigned(const QString& sessionId) {
    const auto working = host_->agentConversation(sessionId);
    if (!working)
        return;
    QPointer<AgentBroker> self(this);
    snapshotWorktree(sessionId, working->worktree, [self, sessionId](ReviewSnapshot snapshot) {
        if (self)
            self->reviewSnapshots_.insert(sessionId, snapshot);
    });
}

void AgentBroker::reviewFinishedTurn(const QString& sessionId, const qint64 generation,
                                     const QString& status, const TurnOrigin& origin) {
    const auto reviewer = journal_->conversationReviewer(sessionId);
    const auto working = host_->agentConversation(sessionId);
    if (!reviewer || !working || working->generation != generation)
        return;
    const bool completed = status.isEmpty() || status == QStringLiteral("completed");
    const ReviewSnapshot previous = reviewSnapshots_.value(sessionId);
    QPointer<AgentBroker> self(this);
    const QString base = previous.base.isEmpty() ? QStringLiteral("HEAD") : previous.base;
    // Collect the turn's changes, then move the baseline so the next turn is compared with now.
    runGit(this, working->worktree,
           {QStringLiteral("diff"), QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"),
            base},
           [self, sessionId, generation, completed, origin, previous,
            base](std::optional<QString> diff) {
               if (!self)
                   return;
               const auto conversation = self->host_->agentConversation(sessionId);
               if (!conversation)
                   return;
               self->snapshotWorktree(
                   sessionId, conversation->worktree,
                   [self, sessionId, generation, completed, origin, previous, base,
                    diff](ReviewSnapshot current) {
                       if (!self)
                           return;
                       self->reviewSnapshots_.insert(sessionId, current);
                       if (!completed)
                           return; // Cancelled, failed or stopped turns trigger nothing.
                       QStringList added;
                       for (const QString& file : std::as_const(current.untracked))
                           if (!previous.untracked.contains(file))
                               added.append(file);
                       added.sort();
                       self->postReview(sessionId, generation, origin, base,
                                        diff.value_or(QString{}), added);
                   });
           });
}

void AgentBroker::postReview(const QString& sessionId, const qint64 generation,
                             const TurnOrigin& origin, const QString& base, const QString& diff,
                             const QStringList& added) {
    const auto reviewer = journal_->conversationReviewer(sessionId);
    const auto working = host_->agentConversation(sessionId);
    if (!reviewer || !working || working->generation != generation)
        return;
    const auto skip = [&](const QString& reason) {
        host_->appendAgentNotice(sessionId, generation,
                                 {{QStringLiteral("kind"), QStringLiteral("review")},
                                  {QStringLiteral("state"), QStringLiteral("review_skipped")},
                                  {QStringLiteral("reason"), reason},
                                  {QStringLiteral("direction"), QStringLiteral("outgoing")}});
    };
    if (diff.trimmed().isEmpty() && added.isEmpty())
        return skip(QStringLiteral("no_changes"));
    const int round = journal_->reviewRequestsInChain(origin.chainId, sessionId) + 1;
    if (round > reviewer->maxRounds)
        return skip(QStringLiteral("round_limit"));
    const auto target = host_->agentConversation(reviewer->reviewerSessionId);
    if (!target)
        return skip(QStringLiteral("reviewer_missing"));
    if (!target->idle)
        return skip(QStringLiteral("reviewer_busy"));
    AgentMessageRecord message;
    message.id = newId();
    message.chainId = origin.chainId;
    message.hop = origin.hop + 1;
    message.fromSession = sessionId;
    message.fromGeneration = generation;
    message.fromNumber = working->number;
    message.fromTitle = working->title;
    message.fromProvider = working->provider;
    message.toSession = target->sessionId;
    message.toGeneration = target->generation;
    message.toNumber = target->number;
    message.kind = QStringLiteral("review");
    message.expectReply = true;
    message.body = reviewBody(*working, base, round, reviewer->maxRounds,
                              userRequests_.value(sessionId), diff, added);
    message.state = QStringLiteral("queued");
    message.createdAt = nowIso();
    message.expiresAt = QDateTime::currentDateTimeUtc()
                            .addSecs(qint64{requestLifetimeHours} * 3600)
                            .toString(Qt::ISODateWithMs);
    if (const QString error = post(message); !error.isEmpty()) {
        qWarning("Unable to request a review of %s: %s", qUtf8Printable(sessionId),
                 qUtf8Printable(error));
        return skip(QStringLiteral("journal_error"));
    }
    host_->appendAgentNotice(sessionId, generation,
                             {{QStringLiteral("message_id"), message.id},
                              {QStringLiteral("kind"), message.kind},
                              {QStringLiteral("state"), QStringLiteral("sent")},
                              {QStringLiteral("delivery"), QStringLiteral("starting")},
                              {QStringLiteral("expect_reply"), true},
                              {QStringLiteral("round"), round},
                              {QStringLiteral("max_rounds"), reviewer->maxRounds},
                              {QStringLiteral("direction"), QStringLiteral("outgoing")},
                              {QStringLiteral("peer_number"), target->number},
                              {QStringLiteral("peer_title"), target->title},
                              {QStringLiteral("peer_provider"), target->provider}});
}

bool AgentBroker::recordReviewReply(AgentMessageRecord& reply, const AgentMessageRecord& request) {
    if (request.kind != QStringLiteral("review"))
        return false;
    const auto reviewer = journal_->conversationReviewer(request.fromSession);
    const bool noFindings = reportsNoFindings(reply.body);
    const bool fix =
        !noFindings && reviewer && reviewer->autoFix &&
        journal_->reviewRequestsInChain(request.chainId, request.fromSession) < reviewer->maxRounds;
    if (fix)
        return false; // Delivered as usual: the idle working agent starts a turn on it.
    // Shown in the working conversation without spending another turn.
    reply.state = QStringLiteral("delivered");
    reply.delivery = QStringLiteral("review_record");
    reply.deliveredAt = nowIso();
    QString error;
    if (!journal_->insertAgentMessage(reply, &error)) {
        qWarning("Unable to record the review of %s: %s", qUtf8Printable(request.fromSession),
                 qUtf8Printable(error));
        return true;
    }
    host_->appendAgentNotice(request.fromSession, request.fromGeneration,
                             {{QStringLiteral("message_id"), reply.id},
                              {QStringLiteral("kind"), reply.kind},
                              {QStringLiteral("state"), QStringLiteral("review_recorded")},
                              {QStringLiteral("reply_to"), request.id},
                              {QStringLiteral("verdict"), noFindings ? QStringLiteral("no_findings")
                                                                     : QStringLiteral("findings")},
                              {QStringLiteral("body"), reply.body},
                              {QStringLiteral("direction"), QStringLiteral("incoming")},
                              {QStringLiteral("peer_number"), reply.fromNumber},
                              {QStringLiteral("peer_title"), reply.fromTitle}});
    return true;
}

} // namespace ketplus::daemon
