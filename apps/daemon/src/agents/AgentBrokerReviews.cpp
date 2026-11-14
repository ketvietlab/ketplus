// ADR 0067: automatic review of a conversation's finished turns by its chosen reviewer.
#include "agents/AgentBroker.h"
#include "agents/GitProcess.h"
#include "agents/ReviewVerdict.h"
#include "agents/ReviewerPolicy.h"
#include "storage/LocalJournal.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTimer>
#include <QUuid>

#include <functional>

namespace ketplus::daemon {
namespace {
constexpr qsizetype userRequestBytes = 4 * 1024;
constexpr qsizetype fileListBytes = 4 * 1024;

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

qsizetype utf8Size(const QString& text) { return text.toUtf8().size(); }

// `git diff --name-status` lines as "path (added)", "old → new (renamed)" and so on.
QStringList changedFiles(const QString& nameStatus) {
    QStringList files;
    for (const QString& line : nameStatus.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList parts = line.split(QLatin1Char('\t'));
        if (parts.size() < 2)
            continue;
        const QChar kind = parts.first().at(0);
        const QString path =
            parts.size() > 2 ? parts.at(1) + QStringLiteral(" → ") + parts.at(2) : parts.at(1);
        const QString label = kind == QLatin1Char('A')   ? QStringLiteral(" (added)")
                              : kind == QLatin1Char('D') ? QStringLiteral(" (deleted)")
                              : kind == QLatin1Char('R') ? QStringLiteral(" (renamed)")
                                                         : QString{};
        files.append(path + label);
    }
    return files;
}

// One `diff --git` section per file, in order.
QStringList fileSections(const QString& diff) {
    static const QRegularExpression header(QStringLiteral("^diff --git "),
                                           QRegularExpression::MultilineOption);
    QStringList sections;
    qsizetype start = -1;
    for (auto match = header.globalMatch(diff); match.hasNext();) {
        const qsizetype at = match.next().capturedStart();
        if (start >= 0)
            sections.append(diff.mid(start, at - start));
        start = at;
    }
    if (start >= 0)
        sections.append(diff.mid(start));
    return sections;
}

QString sectionPath(const QString& section) {
    static const QRegularExpression header(QStringLiteral("^diff --git a/.+ b/(.+)$"),
                                           QRegularExpression::MultilineOption);
    const auto match = header.match(section);
    return match.hasMatch() ? match.captured(1) : QStringLiteral("(unknown file)");
}

// The whole request stays within the 32 KiB message limit; whatever does not fit is named.
QString reviewBody(const AgentConversation& working, const QString& base, const int round,
                   const int rounds, const QString& userRequest, const QString& diff,
                   const QStringList& files) {
    QStringList listed;
    qsizetype listBytes = 0;
    for (const QString& file : files) {
        if (listBytes + utf8Size(file) + 2 > fileListBytes)
            break;
        listed.append(file);
        listBytes += utf8Size(file) + 2;
    }
    QString fileList = listed.join(QStringLiteral(", "));
    if (listed.size() < files.size())
        fileList += QStringLiteral(" and %1 more").arg(files.size() - listed.size());
    const QString header =
        QStringList{QStringLiteral("Automatic review request from #%1 \"%2\" (round %3 of %4).")
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
                        .arg(files.isEmpty() ? QStringLiteral("none") : fileList)}
            .join(QLatin1Char('\n'));
    const QString closing =
        QStringLiteral("Review these changes read-only. For each finding give the file, line, "
                       "severity and a one-line summary, or say there are none. End with exactly "
                       "one line: \"Verdict: findings\" or \"Verdict: no findings\".");
    const QString fenceOpen = QStringLiteral("\n\n```diff\n");
    const QString fenceClose = QStringLiteral("\n```");
    // Room for the note naming what was left out, which itself is cut to fit.
    constexpr qsizetype noteBytes = 2 * 1024;
    const qsizetype budget = AgentBroker::maximumBodyBytes - utf8Size(header) - utf8Size(closing) -
                             utf8Size(fenceOpen) - utf8Size(fenceClose) - noteBytes - 8;
    QString shown;
    QStringList cut;
    QStringList omitted;
    for (const QString& section : fileSections(diff)) {
        if (!omitted.isEmpty() || !cut.isEmpty()) {
            omitted.append(sectionPath(section));
            continue;
        }
        if (utf8Size(shown) + utf8Size(section) <= budget) {
            shown += section;
            continue;
        }
        // A section that does not fit is shown up to the budget and named as cut.
        shown += utf8Prefix(section, budget - utf8Size(shown));
        cut.append(sectionPath(section));
    }
    QString body = header;
    if (!shown.trimmed().isEmpty())
        body += fenceOpen + shown.trimmed() + fenceClose;
    if (!cut.isEmpty() || !omitted.isEmpty()) {
        QString note = QStringLiteral("\n\nThe diff was shortened to fit one message. Read these "
                                      "files in the worktree: ") +
                       (cut + omitted).join(QStringLiteral(", "));
        body += utf8Prefix(note, noteBytes);
    }
    return body + QStringLiteral("\n\n") + closing;
}

} // namespace

bool AgentBroker::canReview(const AgentConversation& conversation) {
    return reviewer::supported(conversation.provider, conversation.executionPolicy);
}

bool AgentBroker::verdictHasNoFindings(const QString& text) {
    return reviewVerdict(text) == ReviewVerdict::noFindings;
}

// A snapshot is the tree of every tracked and untracked (not ignored) file, written through
// a copy of the worktree's index so neither the worktree nor its index changes.
void AgentBroker::snapshotWorktree(const QString& worktree,
                                   std::function<void(std::optional<QString>)> done) {
    QPointer<AgentBroker> self(this);
    runGit(this, worktree,
           {QStringLiteral("rev-parse"), QStringLiteral("--git-path"), QStringLiteral("index")},
           [self, worktree, done](std::optional<QString> indexPath) {
               if (!self || !indexPath)
                   return done(std::nullopt);
               auto index = std::make_shared<QTemporaryFile>(
                   QDir::temp().filePath(QStringLiteral("ketplus-review-XXXXXX.index")));
               if (!index->open())
                   return done(std::nullopt);
               index->close();
               // A new repository has no index yet; git then starts from an empty one.
               const QString source = QDir(worktree).absoluteFilePath(indexPath->trimmed());
               QFile::remove(index->fileName());
               if (QFileInfo::exists(source) && !QFile::copy(source, index->fileName()))
                   return done(std::nullopt);
               runGit(
                   self, worktree, {QStringLiteral("add"), QStringLiteral("-A")},
                   [self, worktree, done, index](std::optional<QString> added) {
                       if (!self || !added)
                           return done(std::nullopt);
                       runGit(
                           self, worktree, {QStringLiteral("write-tree")},
                           [done, index](std::optional<QString> tree) {
                               done(tree && !tree->trimmed().isEmpty()
                                        ? std::optional<QString>(tree->trimmed())
                                        : std::nullopt);
                           },
                           index->fileName());
                   },
                   index->fileName());
           });
}

void AgentBroker::noteReviewerAssigned(const QString& sessionId) {
    const auto working = host_->agentConversation(sessionId);
    if (!working)
        return;
    const qint64 epoch = ++snapshotEpochs_[sessionId];
    QPointer<AgentBroker> self(this);
    snapshotWorktree(working->worktree, [self, sessionId, epoch](std::optional<QString> tree) {
        // A later snapshot of this conversation has already replaced this one.
        if (!self || self->snapshotEpochs_.value(sessionId) != epoch)
            return;
        if (tree)
            self->reviewSnapshots_.insert(sessionId, *tree);
        else
            self->reviewSnapshots_.remove(sessionId);
    });
}

void AgentBroker::reviewFinishedTurn(const QString& sessionId, const qint64 generation,
                                     const QString& status, const TurnOrigin& origin) {
    const auto reviewer = journal_->conversationReviewer(sessionId);
    const auto working = host_->agentConversation(sessionId);
    if (!reviewer || !working || working->generation != generation)
        return;
    // ADR 0114: the team's reviewer reviews a member's tasks instead.
    if (host_->onTeam(sessionId))
        return;
    // Cancelled, failed or stopped turns trigger nothing; their changes stay in the next diff.
    if (!status.isEmpty() && status != QStringLiteral("completed"))
        return;
    const QString base = reviewSnapshots_.value(sessionId, QStringLiteral("HEAD"));
    const qint64 epoch = ++snapshotEpochs_[sessionId];
    QPointer<AgentBroker> self(this);
    const auto failed = [self, sessionId, generation] {
        if (self)
            self->host_->appendAgentNotice(
                sessionId, generation,
                {{QStringLiteral("kind"), QStringLiteral("review")},
                 {QStringLiteral("state"), QStringLiteral("review_skipped")},
                 {QStringLiteral("reason"), QStringLiteral("snapshot_failed")},
                 {QStringLiteral("direction"), QStringLiteral("outgoing")}});
    };
    // Both ends of the diff are fixed trees, so later edits cannot move into this review.
    snapshotWorktree(working->worktree, [self, sessionId, generation, origin, base, epoch,
                                         failed](std::optional<QString> tree) {
        if (!self)
            return;
        const auto conversation = self->host_->agentConversation(sessionId);
        if (!conversation)
            return;
        if (!tree)
            return failed(); // The baseline stays, so the next turn still covers these changes.
        const QString worktree = conversation->worktree;
        const QString current = *tree;
        runGit(self, worktree,
               {QStringLiteral("diff"), QStringLiteral("--name-status"), QStringLiteral("-M"), base,
                current},
               [self, sessionId, generation, origin, base, epoch, failed, worktree,
                current](std::optional<QString> names) {
                   if (!self)
                       return;
                   if (!names)
                       return failed();
                   runGit(self, worktree,
                          {QStringLiteral("diff"), QStringLiteral("--no-color"),
                           QStringLiteral("--no-ext-diff"), QStringLiteral("-M"), base, current},
                          [self, sessionId, generation, origin, base, epoch, failed, current,
                           names](std::optional<QString> diff) {
                              if (!self)
                                  return;
                              if (!diff)
                                  return failed();
                              if (self->snapshotEpochs_.value(sessionId) == epoch)
                                  self->reviewSnapshots_.insert(sessionId, current);
                              self->postReview(sessionId, generation, origin, base, *diff,
                                               changedFiles(*names));
                          });
               });
    });
}

void AgentBroker::postReview(const QString& sessionId, const qint64 generation,
                             const TurnOrigin& origin, const QString& base, const QString& diff,
                             const QStringList& files) {
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
    if (diff.trimmed().isEmpty() && files.isEmpty())
        return skip(QStringLiteral("no_changes"));
    const int round = journal_->reviewRequestsInChain(origin.chainId, sessionId) + 1;
    if (round > reviewer->maxRounds)
        return skip(QStringLiteral("round_limit"));
    const auto target = host_->agentConversation(reviewer->reviewerSessionId);
    if (!target)
        return skip(QStringLiteral("reviewer_missing"));
    if (!canReview(*target))
        return skip(QStringLiteral("reviewer_unsupported"));
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
                              userRequests_.value(sessionId), diff, files);
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
    // T-19: only findings start a fix turn; a reply without a verdict line is recorded as
    // "no verdict" and sends nothing to fix.
    const ReviewVerdict verdict = reviewVerdict(reply.body);
    const bool fix =
        verdict == ReviewVerdict::findings && reviewer && reviewer->autoFix &&
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
                              {QStringLiteral("verdict"), verdictName(verdict)},
                              {QStringLiteral("body"), reply.body},
                              {QStringLiteral("direction"), QStringLiteral("incoming")},
                              {QStringLiteral("peer_number"), reply.fromNumber},
                              {QStringLiteral("peer_title"), reply.fromTitle}});
    return true;
}

} // namespace ketplus::daemon
