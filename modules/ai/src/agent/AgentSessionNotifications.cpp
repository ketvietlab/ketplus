#include "agent/AgentSessionManager.h"
#include "agent/DaemonTranscriptNotices.h"

#include "agent/CodexAppServer.h"
#include "agent/CodexRateLimits.h"
#include "agent/DaemonTranscriptProjection.h"
#include <QDateTime>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSet>
#include <QSettings>
#include <QUrl>
#include <QUuid>

#include "agent/AgentMessageFormatter.h"
#include "agent/AgentSessionState.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/McpToolResultMedia.h"
#include "agent/TranscriptNormalizer.h"

#include <algorithm>
#include <optional>

namespace ketplus {
using namespace agent_message;

namespace {

QString stableOptionId(const QString& questionId, const int index, const QString& label) {
    return QStringLiteral("option_%1")
        .arg(QString::fromLatin1(
            QCryptographicHash::hash(
                QStringLiteral("%1\n%2\n%3").arg(questionId).arg(index).arg(label).toUtf8(),
                QCryptographicHash::Sha256)
                .toHex()
                .left(24)));
}

std::optional<QVariantList> normalizedQuestions(const QJsonArray& source) {
    // Claude asks up to four questions at once; Codex up to three.
    if (source.isEmpty() || source.size() > 4)
        return std::nullopt;
    QVariantList questions;
    QSet<QString> questionIds;
    for (const auto& value : source) {
        if (!value.isObject())
            return std::nullopt;
        const QJsonObject sourceQuestion = value.toObject();
        const QString id = sourceQuestion.value(QStringLiteral("id")).toString().trimmed();
        QString text = sourceQuestion.value(QStringLiteral("question")).toString().trimmed();
        if (text.isEmpty())
            text = sourceQuestion.value(QStringLiteral("prompt")).toString().trimmed();
        const QString header = sourceQuestion.value(QStringLiteral("header")).toString();
        const QString selection = sourceQuestion.value(QStringLiteral("selection")).toString();
        if (id.isEmpty() || id.size() > 128 || questionIds.contains(id) || text.isEmpty() ||
            text.size() > 4096 || header.size() > 80 ||
            (!selection.isEmpty() && selection != QStringLiteral("single") &&
             selection != QStringLiteral("multiple")))
            return std::nullopt;
        const QJsonArray sourceOptions = sourceQuestion.value(QStringLiteral("options")).toArray();
        if ((sourceOptions.isEmpty() && !sourceQuestion.value("isOther").toBool() &&
             !sourceQuestion.value("allow_custom").toBool()) ||
            sourceOptions.size() > 20)
            return std::nullopt;
        QVariantList options;
        QSet<QString> optionIds;
        for (int optionIndex = 0; optionIndex < sourceOptions.size(); ++optionIndex) {
            const QJsonValue optionValue = sourceOptions.at(optionIndex);
            if (!optionValue.isObject())
                return std::nullopt;
            const QJsonObject sourceOption = optionValue.toObject();
            const QString label = sourceOption.value(QStringLiteral("label")).toString().trimmed();
            QString optionId = sourceOption.value(QStringLiteral("id")).toString().trimmed();
            if (optionId.isEmpty())
                optionId = stableOptionId(id, optionIndex, label);
            const QString description =
                sourceOption.value(QStringLiteral("description")).toString();
            if (label.isEmpty() || label.size() > 256 || optionId.size() > 128 ||
                description.size() > 1024 || optionIds.contains(optionId))
                return std::nullopt;
            optionIds.insert(optionId);
            options.append(QVariantMap{{QStringLiteral("id"), optionId},
                                       {QStringLiteral("label"), label},
                                       {QStringLiteral("description"), description}});
        }
        questionIds.insert(id);
        questions.append(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("revision"), 1},
            {QStringLiteral("header"), header},
            {QStringLiteral("question"), text},
            {QStringLiteral("options"), options},
            {QStringLiteral("selection"),
             selection.isEmpty() && sourceQuestion.value(QStringLiteral("multiSelect")).toBool()
                 ? QStringLiteral("multiple")
             : selection.isEmpty() ? QStringLiteral("single")
                                   : selection},
            {QStringLiteral("isOther"),
             sourceQuestion.value(QStringLiteral("isOther")).toBool() ||
                 sourceQuestion.value(QStringLiteral("allow_custom")).toBool()},
            {QStringLiteral("isSecret"),
             sourceQuestion.value(QStringLiteral("isSecret")).toBool() ||
                 sourceQuestion.value(QStringLiteral("secret")).toBool()}});
    }
    return questions;
}

} // namespace

void AgentSessionManager::handleUserInputRequest(const QJsonValue& requestId,
                                                 const QJsonObject& params) {
    const SessionPtr session =
        sessionForThread(params.value(QStringLiteral("threadId")).toString());
    const QString itemId = params.value(QStringLiteral("itemId")).toString();
    const std::optional<QVariantList> questions =
        normalizedQuestions(params.value(QStringLiteral("questions")).toArray());
    if (!questions.has_value()) {
        server_->rejectUserInput(requestId, QStringLiteral("invalid_question_request"),
                                 QStringLiteral("The structured question request is invalid."));
        return;
    }
    if (!session || itemId.isEmpty() || session->messageIndexByItem.contains(itemId) ||
        (!server_->usesDaemon() && !session->turnId.isEmpty() &&
         params.value(QStringLiteral("turnId")).toString() != session->turnId)) {
        // A detached, stale or duplicate UI is never a user decision. The daemon
        // retains the request and returns it when this conversation is restored.
        if (!server_->usesDaemon())
            server_->respondToUserInput(requestId, {});
        return;
    }

    const bool approval =
        params.value(QStringLiteral("approvalKind")).toString() == QStringLiteral("approval");
    auto entry =
        message(QStringLiteral("tool"), {},
                approval ? QStringLiteral("Agent · approval") : QStringLiteral("Agent · question"),
                {}, itemId);
    entry.insert(QStringLiteral("subtype"),
                 approval ? QStringLiteral("approval") : QStringLiteral("question"));
    entry.insert(QStringLiteral("running"), true);
    entry.insert(QStringLiteral("blocking"),
                 params.value(QStringLiteral("isBlocking")).toBool(true));
    entry.insert(QStringLiteral("questions"), *questions);
    entry.insert(QStringLiteral("questionAnswers"), QVariantMap{});
    if (approval) {
        entry.insert(QStringLiteral("body"),
                     params.value(QStringLiteral("approvalReason")).toString());
        entry.insert(QStringLiteral("command"),
                     params.value(QStringLiteral("approvalCommand")).toString());
    }
    const int index = appendMessage(session, entry);
    session->messageIndexByItem.insert(itemId, index);
    const QString messageKey =
        session->messages.at(index).value(QStringLiteral("messageKey")).toString();
    session->pendingQuestionRequests.insert(messageKey, requestId);
    if (entry.value(QStringLiteral("blocking")).toBool())
        updateState(session, QStringLiteral("Waiting for your answer…"), true);
    else
        emit conversationsChanged(session->path, conversations(session->path));
}

void AgentSessionManager::answerQuestion(const QString& worktreePath, const QString& messageKey,
                                         const QVariantMap& answers) {
    const SessionPtr session = sessions_.value(normalizedPath(worktreePath));
    if (!session || !session->pendingQuestionRequests.contains(messageKey)) {
        emit errorOccurred(QStringLiteral("This question is no longer waiting for an answer."));
        return;
    }

    int index = -1;
    for (int candidate = 0; candidate < session->messages.size(); ++candidate) {
        if (session->messages.at(candidate).value(QStringLiteral("messageKey")).toString() ==
            messageKey) {
            index = candidate;
            break;
        }
    }
    if (index < 0) {
        emit errorOccurred(QStringLiteral("The question could not be found in this conversation."));
        return;
    }

    const QVariantMap entry = session->messages.at(index);
    const QVariantList questions = entry.value(QStringLiteral("questions")).toList();
    QJsonObject protocolAnswers;
    QVariantMap displayedAnswers;
    for (const QVariant& value : questions) {
        const QVariantMap question = value.toMap();
        const QString id = question.value(QStringLiteral("id")).toString();
        const QVariantMap answer = answers.value(id).toMap();
        const QStringList optionIds = answer.value(QStringLiteral("option_ids")).toStringList();
        const QString customAnswer =
            answer.value(QStringLiteral("custom_answer")).toString().trimmed();
        if (optionIds.isEmpty() && customAnswer.isEmpty()) {
            emit errorOccurred(QStringLiteral("Answer every question before submitting."));
            return;
        }
        QStringList displayedValues;
        const QVariantList options = question.value(QStringLiteral("options")).toList();
        for (const QString& optionId : optionIds) {
            const auto found =
                std::find_if(options.cbegin(), options.cend(), [&](const QVariant& value) {
                    return value.toMap().value(QStringLiteral("id")).toString() == optionId;
                });
            if (found == options.cend()) {
                emit errorOccurred(QStringLiteral("A selected answer is no longer available."));
                return;
            }
            displayedValues.append(found->toMap().value(QStringLiteral("label")).toString());
        }
        if (!customAnswer.isEmpty())
            displayedValues.append(customAnswer);
        protocolAnswers.insert(
            id, QJsonObject{{QStringLiteral("option_ids"), QJsonArray::fromStringList(optionIds)},
                            {QStringLiteral("custom_answer"), customAnswer}});
        displayedAnswers.insert(id, question.value(QStringLiteral("isSecret")).toBool()
                                        ? QStringLiteral("••••••••")
                                        : displayedValues.join(QStringLiteral(", ")));
    }

    server_->respondToUserInput(session->pendingQuestionRequests.take(messageKey), protocolAnswers,
                                QJsonArray::fromVariantList(questions));
    QVariantMap answered = entry;
    answered.insert(QStringLiteral("running"), false);
    answered.insert(QStringLiteral("meta"), QStringLiteral("Codex · answered"));
    answered.insert(QStringLiteral("questionAnswers"), displayedAnswers);
    updateMessage(session, index, answered);
    if (entry.value(QStringLiteral("blocking")).toBool())
        updateState(session, QStringLiteral("Working…"), true);
    else
        emit conversationsChanged(session->path, conversations(session->path));
}

void AgentSessionManager::handleNotification(const QString& method, const QJsonObject& params) {
    QString threadId = params.value(QStringLiteral("threadId")).toString();
    if (threadId.isEmpty()) {
        threadId = params.value(QStringLiteral("thread"))
                       .toObject()
                       .value(QStringLiteral("id"))
                       .toString();
    }
    const SessionPtr session = sessionForThread(threadId);
    if (!session) {
        return;
    }

    if (method == "daemon/inputPending") {
        const auto id = params.value("requestId").toString();
        if (id.isEmpty())
            return;
        const bool pending = params.value("pending").toBool();
        const bool changed = pending != session->pendingInputIds.contains(id);
        if (pending)
            session->pendingInputIds.insert(id);
        else
            session->pendingInputIds.remove(id);
        if (changed)
            emit conversationsChanged(session->path, conversations(session->path));
        return;
    }
    if (method == "daemon/agentTurn") {
        const QJsonObject sender = params.value("sender").toObject();
        session->agentTurnNumber = sender.value("number").toString();
        session->agentTurnTitle = sender.value("title").toString();
        session->agentTurnReply = sender.value("kind").toString() == QStringLiteral("reply");
        emit conversationsChanged(session->path, conversations(session->path));
        return;
    }
    if (method == "daemon/activityGroup") {
        acceptActivityGroup(session, params);
        return;
    }
    if (method == "daemon/configurationChanged") {
        acceptDaemonConfiguration(session, params);
        return;
    }
    if (method == QStringLiteral("thread/name/updated")) {
        const QString title = params.value(QStringLiteral("threadName")).toString().trimmed();
        const bool customTitle = params.value(QStringLiteral("customTitle")).toBool();
        if (title.isEmpty() || (title == session->title && customTitle == session->customTitle)) {
            return;
        }
        session->title = title;
        session->customTitle = customTitle;
        persistWorkspace(session->path);
        emit conversationsChanged(session->path, conversations(session->path));
        if (isActiveSession(session)) {
            emit sessionStateChanged(session->path, session->title, session->status,
                                     session->running);
        }
        return;
    }

    if (method == QStringLiteral("thread/tokenUsage/updated") ||
        method == QStringLiteral("codex/event/token_count")) {
        QJsonObject usage = params.value(QStringLiteral("tokenUsage")).toObject();
        if (usage.isEmpty())
            usage = params.value(QStringLiteral("token_usage")).toObject();
        QJsonObject last = usage.value(QStringLiteral("last")).toObject();
        if (last.isEmpty()) {
            QJsonObject info =
                params.value(QStringLiteral("msg")).toObject().value("info").toObject();
            if (info.isEmpty())
                info = params.value(QStringLiteral("info")).toObject();
            last = info.value(QStringLiteral("last_token_usage")).toObject();
            if (usage.value(QStringLiteral("modelContextWindow")).isUndefined() &&
                usage.value(QStringLiteral("model_context_window")).isUndefined()) {
                usage.insert(QStringLiteral("modelContextWindow"),
                             info.value(QStringLiteral("model_context_window")));
            }
        }
        const qint64 used =
            last.value(QStringLiteral("totalTokens"))
                .toInteger(last.value(QStringLiteral("total_tokens")).toInteger(-1));
        const qint64 limit =
            usage.value(QStringLiteral("modelContextWindow"))
                .toInteger(usage.value(QStringLiteral("model_context_window")).toInteger(0));
        if (used < 0 || limit <= 0)
            return;
        if (session->contextUsedTokens == used && session->contextLimitTokens == limit)
            return;
        session->contextUsedTokens = used;
        session->contextLimitTokens = limit;
        persistWorkspace(session->path);
        emitUsageSnapshot(session);
        return;
    }

    if (method == QStringLiteral("thread/backgroundTasks/updated")) {
        const QVariantList tasks = params.value(QStringLiteral("tasks")).toArray().toVariantList();
        if (session->backgroundTasks == tasks)
            return;
        session->backgroundTasks = tasks;
        emitBackgroundTasks(session);
        return;
    }

    if (method == QStringLiteral("thread/sessionUsage/updated")) {
        const auto usage = params.value("usage").toObject().toVariantMap();
        if (usage.value("mode") != session->sessionUsage.value("mode") ||
            usage.value("mode") == "api_key")
            session->planLimits.clear();
        session->sessionUsage = usage;
        persistWorkspace(session->path);
        emitUsageSnapshot(session);
        return;
    }
    if (method == QStringLiteral("thread/planLimits/updated")) {
        const QVariantList rows = params.value(QStringLiteral("limits")).toArray().toVariantList();
        if (rows.isEmpty() || session->planLimits == rows)
            return;
        session->planLimits = rows;
        persistWorkspace(session->path);
        emitUsageSnapshot(session);
        return;
    }

    if (method == "daemon/turnStartedAt") {
        const qint64 startedAt = daemonTimestamp(params.value("startedAt").toString());
        if (startedAt <= session->lastTurnStartedAt)
            return;
        session->lastTurnStartedAt = startedAt;
        persistWorkspace(session->path);
        emit conversationsChanged(session->path, conversations(session->path));
        return;
    }
    if (method == "daemon/eventScope") {
        const auto sequence = params.value("turnSequence").toInteger();
        if (session->daemonTurnSequence != sequence)
            session->messageIndexByItem.clear();
        session->daemonTurnSequence = sequence;
        // Live turns need durable cursors too: a later older page that trims them away resumes
        // newer paging at their first event.
        const auto cursor = params.value("cursor").toInteger();
        session->daemonLiveEventCursor = cursor;
        if (cursor > 0 && session->historyLoaded) {
            const auto key = daemonTurnId(sequence);
            auto found = session->historyTurnCursors.find(key);
            if (found == session->historyTurnCursors.end())
                session->historyTurnCursors.insert(key, {cursor, cursor});
            else
                *found = {std::min(found->first, cursor), std::max(found->second, cursor)};
        }
        return;
    }
    if (method == "daemon/approvalSyncFailed") {
        session->historyLoaded = false;
        session->historyError = QStringLiteral("Unable to refresh pending approvals: %1")
                                    .arg(params.value("message").toString());
        session->historyErrorNewer = false;
        qWarning().noquote() << "KetPlus:" << session->historyError;
        emitHistoryState(session);
        emit errorOccurred(session->historyError);
        return;
    }
    if (method == "daemon/conversationNumber") {
        const QString number = params.value("number").toString();
        if (number.isEmpty() || session->number == number)
            return;
        session->number = number;
        emit conversationsChanged(session->path, conversations(session->path));
        return;
    }
    if (method == "daemon/providerNotice") {
        const auto itemId = params.value("itemId").toString();
        if (itemId.isEmpty() || messageIndexForItem(session, itemId) >= 0)
            return;
        const auto title = params.value("title").toString();
        auto entry =
            title.isEmpty()
                ? agent_message::providerNoticeMessage(params.value("text").toString(), itemId)
                : agent_message::providerNoticeMessage(params.value("text").toString(), itemId,
                                                       title);
        if (params.value("presentation").isObject()) {
            entry = params.value("presentation").toObject().toVariantMap();
            if (entry.value("subtype") == "turnEnd") {
                // The same line a reloaded window projects from the journal: the daemon
                // times and sizes the turn, the session supplies its running cost.
                agent_message::TurnFacts turn;
                turn.durationMs = entry.value("durationMs", -1).toLongLong();
                turn.contextTokens = entry.value("contextTokens", -1).toLongLong();
                const auto detail = agent_message::turnEndText(
                    turn, QJsonObject::fromVariantMap(session->sessionUsage));
                entry.insert("body", detail);
                entry.insert("summary", detail);
            }
        }
        if (!entry.contains("turnId") && !session->turnId.isEmpty())
            entry.insert(QStringLiteral("turnId"), session->turnId);
        session->messageIndexByItem.insert(itemId, appendMessage(session, entry));
        return;
    }
    if (method == "daemon/userMessage") {
        const auto itemId = params.value("itemId").toString();
        if (itemId.isEmpty() || messageIndexForItem(session, itemId) >= 0)
            return;
        const auto clientMessageId = params.value("clientMessageId").toString();
        const auto text = params.value("text").toString();
        // Prefer the exact client message id. A daemon that journals ids (every daemon with
        // transcript windows) sends an id-less echo only for prompts from another client such
        // as mobile or the web console; those must not claim a local prompt with the same text.
        // The text fallback is kept for rows without an id and for legacy daemons that never
        // echo ids.
        const bool echoesIds = server_->hasDaemonTranscriptWindow();
        int match = -1;
        for (int index = session->messages.size() - 1; index >= 0; --index) {
            const auto& entry = session->messages.at(index);
            if (entry.value("kind") != "user" || !entry.value("itemId").toString().isEmpty())
                continue;
            const auto localId = entry.value("clientMessageId").toString();
            if (clientMessageId.isEmpty()
                    ? entry.value("body").toString() == text && (localId.isEmpty() || !echoesIds)
                    : localId == clientMessageId) {
                match = index;
                break;
            }
        }
        if (match < 0) {
            handleItem(session,
                       {{QStringLiteral("id"), itemId},
                        {QStringLiteral("type"), QStringLiteral("daemonUserMessage")},
                        {QStringLiteral("timestamp"), params.value("timestamp")},
                        {QStringLiteral("eventType"), params.value("eventType")},
                        {QStringLiteral("text"), text},
                        {QStringLiteral("images"), params.value("images")},
                        {QStringLiteral("agentMessages"), params.value("agentMessages")}},
                       true);
            return;
        }
        auto& entry = session->messages[match];
        entry.insert("itemId", itemId);
        entry.insert("timestamp", params.value("timestamp").toString());
        // The daemon's copy outlives the composer's attachment files.
        const auto images = mcp_media::messageImages(params.value("images").toArray());
        if (!images.isEmpty())
            entry.insert("images", images);
        if (server_->hasDaemonTranscriptWindow()) {
            entry.insert("messageKey", daemonMessageKey(session->daemonTurnSequence, itemId));
            entry.insert("turnId", daemonTurnId(session->daemonTurnSequence));
        }
        session->messageIndexByItem.insert(itemId, match);
        if (isActiveSession(session))
            emit messageUpdated(session->path, match, entry);
        return;
    }

    if (method == "daemon/approvalEnded") {
        const auto approvalId = params.value("approvalId");
        const auto pendingCount = session->pendingQuestionRequests.size();
        for (auto it = session->pendingQuestionRequests.begin();
             it != session->pendingQuestionRequests.end();) {
            if (it.value() != approvalId) {
                ++it;
                continue;
            }
            const auto key = it.key();
            it = session->pendingQuestionRequests.erase(it);
            for (int index = 0; index < session->messages.size(); ++index) {
                auto entry = session->messages.at(index);
                if (entry.value("messageKey") != key)
                    continue;
                entry.insert("running", false);
                entry.insert("meta", "Codex · question ended");
                updateMessage(session, index, entry);
                break;
            }
        }
        if (session->pendingQuestionRequests.size() != pendingCount) {
            if (session->pendingQuestionRequests.isEmpty() && session->running &&
                session->status == QStringLiteral("Waiting for your answer…"))
                updateState(session, QStringLiteral("Working…"), true);
            else
                emit conversationsChanged(session->path, conversations(session->path));
        }
        return;
    }

    if (method == QStringLiteral("userInput/resolved")) {
        const auto requestId = params.value(QStringLiteral("requestId"));
        QString key;
        for (auto it = session->pendingQuestionRequests.cbegin();
             it != session->pendingQuestionRequests.cend(); ++it) {
            if (it.value() == requestId) {
                key = it.key();
                break;
            }
        }
        if (key.isEmpty())
            return;
        session->pendingQuestionRequests.remove(key);
        for (int index = 0; index < session->messages.size(); ++index) {
            auto entry = session->messages.at(index);
            if (entry.value(QStringLiteral("messageKey")).toString() != key)
                continue;
            entry.insert(QStringLiteral("running"), false);
            entry.insert(QStringLiteral("status"), params.value(QStringLiteral("expired")).toBool()
                                                       ? QStringLiteral("expired")
                                                       : QStringLiteral("resolved"));
            entry.insert(QStringLiteral("meta"), QStringLiteral("Question no longer pending"));
            updateMessage(session, index, entry);
            break;
        }
        if (session->pendingQuestionRequests.isEmpty())
            updateState(session,
                        session->turnId.isEmpty() ? QStringLiteral("Ready")
                                                  : QStringLiteral("Working…"),
                        !session->turnId.isEmpty());
        return;
    }

    if (method == QStringLiteral("turn/started")) {
        if (server_->hasDaemonTranscriptWindow())
            session->messageIndexByItem.clear();
        session->turnId =
            params.value(QStringLiteral("turn")).toObject().value(QStringLiteral("id")).toString();
        session->loadedTurns.insert(session->turnId);
        updateState(session, QStringLiteral("Working…"), true);
        if (session->cancelRequested) {
            cancelSession(session);
        }
        snapshotTurnWorktrees(session);
        return;
    }
    if (method == QStringLiteral("item/agentMessage/delta")) {
        const QString turnId = params.value(QStringLiteral("turnId")).toString();
        if (!turnId.isEmpty() && turnId != session->turnId)
            return;
        appendAgentDelta(session, params.value(QStringLiteral("itemId")).toString(),
                         params.value(QStringLiteral("delta")).toString());
        return;
    }
    if (method == QStringLiteral("item/commandExecution/outputDelta")) {
        const QString itemId = params.value(QStringLiteral("itemId")).toString();
        const int index = messageIndexForItem(session, itemId);
        if (index < 0 || index >= session->messages.size())
            return;
        QVariantMap entry = session->messages.at(index);
        if (entry.value(QStringLiteral("command")).toString().isEmpty())
            return;
        QString output = entry.value(QStringLiteral("details"))
                             .toMap()
                             .value(QStringLiteral("output"))
                             .toString();
        output += TranscriptSanitizer::text(params.value(QStringLiteral("delta")).toString());
        constexpr qsizetype maximumOutputCharacters = 128 * 1024;
        constexpr auto truncated = "[Earlier command output truncated]\n";
        if (output.size() > maximumOutputCharacters)
            output = QString::fromLatin1(truncated) +
                     output.right(maximumOutputCharacters - qstrlen(truncated));
        QVariantMap details = entry.value(QStringLiteral("details")).toMap();
        details.insert(QStringLiteral("output"), output);
        entry.insert(QStringLiteral("details"), details);
        updateMessage(session, index, entry);
        return;
    }
    if (method == QStringLiteral("item/started") || method == QStringLiteral("item/completed")) {
        auto item = params.value(QStringLiteral("item")).toObject();
        if (item.value(QStringLiteral("type")).toString() == QStringLiteral("contextCompaction")) {
            const auto turnId = params.value(QStringLiteral("turnId")).toString();
            if (!session->running || (!turnId.isEmpty() && turnId != session->turnId))
                return;
            // Daemon windows already scope rows by turn sequence. Keep their canonical item
            // id so a completion loaded from a window closes the same live operation.
            if (!server_->hasDaemonTranscriptWindow())
                item.insert(QStringLiteral("id"),
                            QStringLiteral("compaction:%1:%2")
                                .arg(session->turnId, item.value(QStringLiteral("id")).toString()));
        }
        handleItem(session, item, method == QStringLiteral("item/completed"));
        return;
    }
    if (method == QStringLiteral("turn/completed")) {
        const QJsonObject turn = params.value(QStringLiteral("turn")).toObject();
        const auto turnId = turn.value(QStringLiteral("id")).toString();
        if (!session->turnId.isEmpty() && !turnId.isEmpty() && session->turnId != turnId)
            return;
        const QString turnStatus = turn.value(QStringLiteral("status")).toString();
        flushAgentDeltas(session);
        if (turnStatus != QStringLiteral("failed"))
            publishDeferredFinal(session);
        session->completed = turnStatus == QStringLiteral("completed");
        session->turnId.clear();
        session->cancelRequested = false;
        if (session->completed && session->lastEventCursor <= session->lastSeenCursor)
            session->lastEventCursor = session->lastSeenCursor + 1;
        // Viewing the completed turn keeps it read; background completions stay unread.
        if (session->completed && sessions_.value(session->path) == session)
            markConversationSeen(session->path, session->id);
        if (turnStatus == QStringLiteral("failed")) {
            // Daemon Cursor/Claude turns emit runtime.error then turn.completed
            // failed with no error object. failSession already ran for the
            // first event; stringifying the empty object would add a `{}` row.
            if (session->running)
                failSession(session, errorMessage(turn.value(QStringLiteral("error")).toObject()));
            else
                updateState(session, QStringLiteral("Error"), false);
        } else if (turnStatus == QStringLiteral("interrupted")) {
            updateState(session, QStringLiteral("Stopped"), false);
        } else {
            updateState(session, QStringLiteral("Ready"), false);
        }
        session->agentMessagePhaseByItem.clear();
        session->deferredAgentTextByItem.clear();
        session->daemonDeferredDeltasByItem.clear();
        session->deferredAgentOrder.clear();
        session->explicitFinalInTurn = false;
        captureTurnWorktrees(session);
        persistWorkspace(session->path);
        return;
    }
    if (method == QStringLiteral("error")) {
        const QString detail = errorMessage(params.value(QStringLiteral("error")).toObject());
        const bool replayed = params.value(QStringLiteral("replayed")).toBool();
        if (replayed || params.value(QStringLiteral("nonFatal")).toBool()) {
            // History replay already restored the session state, and non-fatal failures leave
            // the turn running: keep the transcript row without ending the turn.
            recordSessionError(session, detail, !replayed);
            return;
        }
        failSession(session, detail);
    }
}

void AgentSessionManager::handleItem(const SessionPtr& session, const QJsonObject& item,
                                     const bool completed) {
    const QString type = item.value(QStringLiteral("type")).toString();
    const QString itemId = item.value(QStringLiteral("id")).toString();
    int index = messageIndexForItem(session, itemId);

    if (type == QStringLiteral("daemonUserMessage")) {
        if (index < 0) {
            const auto event =
                ActivityNormalizer::normalize(item, TranscriptPhase::Completed,
                                              {session->id, session->transcriptGeneration,
                                               session->turnId, session->messages.size(), 1});
            index = appendMessage(session, event.toVariantMap());
            session->messageIndexByItem.insert(itemId, index);
        }
        return;
    }
    if (type == QStringLiteral("userMessage")) {
        // Local prompts and acknowledged steering input are inserted at submit time.
        // The protocol item is the same logical message, not a second activity row.
        return;
    }

    if (type == QStringLiteral("contextCompaction")) {
        if (index >= 0 && !session->messages.at(index).value(QStringLiteral("running")).toBool()) {
            if (completed) {
                session->activeCompactions.remove(itemId);
                updateState(session, QStringLiteral("Working…"), true);
            }
            return;
        }
        const auto event = ActivityNormalizer::normalize(
            item, completed ? TranscriptPhase::Completed : TranscriptPhase::Started,
            {session->id, session->transcriptGeneration, session->turnId,
             index >= 0 ? session->messages.at(index).value(QStringLiteral("sequence")).toLongLong()
                        : session->messages.size(),
             1});
        auto entry = event.toVariantMap();
        if (index < 0) {
            index = appendMessage(session, entry);
            session->messageIndexByItem.insert(itemId, index);
        } else {
            updateMessage(session, index, entry);
        }
        if (completed)
            session->activeCompactions.remove(itemId);
        else
            session->activeCompactions.insert(itemId);
        updateState(session, QStringLiteral("Working…"), true);
        return;
    }

    if (type == QStringLiteral("agentMessage")) {
        const QString phase = item.value(QStringLiteral("phase")).toString();
        session->agentMessagePhaseByItem.insert(itemId, phase);
        const QString text = item.value(QStringLiteral("text")).toString();
        if (phase != QStringLiteral("final_answer") && phase != QStringLiteral("commentary")) {
            if (!session->deferredAgentOrder.contains(itemId))
                session->deferredAgentOrder.append(itemId);
            if (!text.isEmpty())
                session->deferredAgentTextByItem.insert(itemId, text);
            return;
        }
        if (completed)
            flushAgentDelta(session, itemId);
        if (phase == QStringLiteral("final_answer"))
            session->explicitFinalInTurn = true;
        session->deferredAgentTextByItem.remove(itemId);
        session->daemonDeferredDeltasByItem.remove(itemId);
        session->deferredAgentOrder.removeAll(itemId);
        const auto event = ActivityNormalizer::normalize(
            item, completed ? TranscriptPhase::Completed : TranscriptPhase::Started,
            {session->id, session->transcriptGeneration, session->turnId,
             index >= 0 ? session->messages.at(index).value(QStringLiteral("sequence")).toLongLong()
                        : session->messages.size(),
             1});
        if (index < 0) {
            index = appendMessage(session, event.toVariantMap());
            session->messageIndexByItem.insert(itemId, index);
        } else if (!text.isEmpty()) {
            updateMessage(session, index, event.toVariantMap());
        }
        return;
    }
    if (type == QStringLiteral("reasoning"))
        updateState(session, completed ? QStringLiteral("Working…") : QStringLiteral("Reasoning…"),
                    true);
    const auto event = ActivityNormalizer::normalize(
        item, completed ? TranscriptPhase::Completed : TranscriptPhase::Started,
        {session->id, session->transcriptGeneration, session->turnId,
         index >= 0 ? session->messages.at(index).value(QStringLiteral("sequence")).toLongLong()
                    : session->messages.size(),
         1});
    QVariantMap entry = event.toVariantMap();
    // A thought's duration runs from when its start arrived to its completion.
    if (type == QStringLiteral("reasoning")) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 startedAt =
            index >= 0
                ? session->messages.at(index).value(QStringLiteral("startedAt"), -1).toLongLong()
                : -1;
        if (!completed)
            entry.insert(QStringLiteral("startedAt"), now);
        else if (startedAt >= 0)
            entry.insert(QStringLiteral("durationMs"), now - startedAt);
    }
    if (entry.value(QStringLiteral("eventType")).toString() == QStringLiteral("image.view")) {
        if (index >= 0) {
            QVariantMap grouped = session->messages.at(index);
            if (mergeImageActivity(grouped, entry, completed)) {
                updateMessage(session, index, grouped);
                return;
            }
        } else if (!session->messages.isEmpty()) {
            const int previousIndex = session->messages.size() - 1;
            QVariantMap grouped = session->messages.at(previousIndex);
            if (mergeImageActivity(grouped, entry, completed)) {
                updateMessage(session, previousIndex, grouped);
                session->messageIndexByItem.insert(itemId, previousIndex);
                return;
            }
        }
    }
    if (index < 0) {
        index = appendMessage(session, entry);
        session->messageIndexByItem.insert(itemId, index);
    } else {
        updateMessage(session, index, entry);
    }
}

void AgentSessionManager::hydrateHistory(const SessionPtr& session, const QJsonObject& thread) {
    const auto prefix = historyMessages(thread.value("turns").toArray(), session->loadedTurns,
                                        session->id, session->transcriptGeneration);
    QVector<QVariantMap> entries;
    for (const auto& value : prefix)
        entries.append(value.toMap());
    session->messages = entries + session->messages;
    for (auto it = session->messageIndexByItem.begin(); it != session->messageIndexByItem.end();
         ++it)
        it.value() += prefix.size();
    for (auto& index : session->loadedIndexByItem)
        index += prefix.size();
    session->historyLoaded = true;
    session->historyLoading = false;
    session->historyError.clear();
    if (isActiveSession(session) && !prefix.isEmpty())
        emit messagesPrepended(session->path, prefix);
    if (!session->running &&
        (session->status == "Saved conversation" || session->status == "Loading conversation…"))
        updateState(session, QStringLiteral("Ready"), false);
    emitHistoryState(session);
}

} // namespace ketplus
