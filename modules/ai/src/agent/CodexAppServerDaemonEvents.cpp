#include "BrowserAgentInstructions.h"
#include "agent/CodexAppServer.h"
#include "agent/DaemonTranscriptNotices.h"

#include "agent/DaemonTranscriptProjection.h"
#include <algorithm>

#include "CodexBrowserTools.h"
#include "CodexLaunchReloader.h"
#include "CodexServerRequests.h"
#include "DaemonClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

#include <utility>

// Daemon mode: journal events and approvals projected into Codex-shaped notifications.

namespace ketplus {

void CodexAppServer::consumeDaemonEvent(const QJsonObject& envelope) {
    const QString sessionId = envelope.value(QStringLiteral("session_id")).toString();
    auto found = daemonSessions_.find(sessionId);
    if (found == daemonSessions_.end()) {
        queueDaemonDiscovery(envelope);
        return;
    }
    if (envelope.value("session_generation").toInteger(found->generation) != found->generation)
        return;
    if (envelope.value("payload").toObject().value("event_type") == "session.deleted") {
        dispatchDaemonEvent(envelope);
        return;
    }
    if (found->replaying) {
        const qint64 cursor = envelope.value(QStringLiteral("cursor")).toInteger();
        found->deferredCursor = std::max(found->deferredCursor, cursor);
        if (daemonHasTranscriptWindow_) {
            if (found->windowOverflow || found->deferredEvents.contains(cursor))
                return;
            const auto bytes = QJsonDocument(envelope).toJson(QJsonDocument::Compact).size();
            if (found->deferredEvents.size() >= 128 || found->deferredBytes + bytes > 512 * 1024) {
                found->windowOverflow = true;
                found->deferredEvents.clear();
                found->deferredBytes = 0;
                return;
            }
            found->deferredBytes += bytes;
        }
        found->deferredEvents.insert(cursor, envelope);
        return;
    }
    dispatchDaemonEvent(envelope);
}

void CodexAppServer::finishDaemonReplay(const QString& sessionId) {
    restoringDaemonSessions_.remove(sessionId);
    const bool scheduledRestore = activeDaemonRestoreSession_ == sessionId;
    if (scheduledRestore) {
        activeDaemonRestoreSession_.clear();
    }
    auto found = daemonSessions_.find(sessionId);
    if (found == daemonSessions_.end()) {
        emit daemonSessionRestoreFinished(sessionId);
        if (scheduledRestore) {
            QTimer::singleShot(0, this, &CodexAppServer::restoreNextDaemonSession);
        } else if (!daemonSessionListInFlight_ && restoringDaemonSessions_.isEmpty()) {
            setRestoring(false);
        }
        return;
    }
    const auto deferred = std::exchange(found->deferredEvents, {});
    found->replaying = false;
    found->replayRequestInFlight = false;
    for (const QJsonObject& event : deferred) {
        dispatchDaemonEvent(event);
    }
    emit daemonSessionRestoreFinished(sessionId);
    if (scheduledRestore) {
        QTimer::singleShot(0, this, &CodexAppServer::restoreNextDaemonSession);
    } else if (!daemonSessionListInFlight_ && restoringDaemonSessions_.isEmpty()) {
        setRestoring(false);
    }
}

void CodexAppServer::dispatchDaemonEvent(const QJsonObject& envelope) {
    const QString sessionId = envelope.value(QStringLiteral("session_id")).toString();
    auto found = daemonSessions_.find(sessionId);
    if (found == daemonSessions_.end()) {
        return;
    }
    const qint64 generation =
        envelope.value(QStringLiteral("session_generation")).toInteger(found->generation);
    const qint64 cursor = envelope.value(QStringLiteral("cursor")).toInteger();
    if (generation != found->generation || cursor <= found->cursor) {
        return;
    }
    found->cursor = cursor;
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    const QString type = payload.value(QStringLiteral("event_type")).toString();
    const QJsonObject data = payload.value(QStringLiteral("data")).toObject();
    const auto emitNotice = [&] {
        auto notice =
            agent_message::daemonEventNotice(type, data, agent_message::daemonNoticeItemId(cursor));
        if (notice.isEmpty())
            return;
        if (daemonHasTranscriptWindow_ &&
            (!found->windowReady || sessionId != daemonRestorePrioritySession_)) {
            if (found->windowReady) {
                found->windowReady = false;
                emit daemonTranscriptInvalidated(sessionId);
            }
            return;
        }
        notice.insert("messageKey",
                      agent_message::daemonMessageKey(envelope.value("turn_sequence").toInteger(),
                                                      notice.value("itemId").toString()));
        notice.insert("turnId",
                      agent_message::daemonTurnId(envelope.value("turn_sequence").toInteger()));
        emit notificationReceived("daemon/providerNotice",
                                  {{"threadId", sessionId},
                                   {"itemId", notice.value("itemId").toString()},
                                   {"presentation", QJsonObject::fromVariantMap(notice)}});
    };
    if (type != "turn.completed")
        emitNotice();

    if (type == "session.deleted") {
        daemonSessions_.remove(sessionId);
        emit daemonSessionDeleted(sessionId, generation);
        return;
    }
    if (type == "session.configuration_changed") {
        const auto effective = data.value("effective").toObject();
        found->options.insert("model", effective.value("model"));
        found->options.insert("effort", effective.value("effort"));
        QJsonObject settings = data;
        settings.insert("threadId", sessionId);
        emit notificationReceived("daemon/configurationChanged", settings);
        return;
    }
    if (type == "conversation.context_changed") {
        found->contextRevision = data.value("context_revision").toInteger(found->contextRevision);
        executionWorktreeCommand(sessionId, "session.worktrees", {{"limit", 1}},
                                 [](const QJsonObject&, const QString&) {});
        return;
    }
    if (type == QStringLiteral("user.message")) {
        // Every client lists conversations by the daemon's turn start, including background
        // ones whose transcript events are skipped below.
        emit notificationReceived(QStringLiteral("daemon/turnStartedAt"),
                                  {{QStringLiteral("threadId"), sessionId},
                                   {QStringLiteral("startedAt"), envelope.value("sent_at")}});
        // The sidebar names the conversation whose message started this turn, if any.
        QJsonObject sender;
        if (data.value(QStringLiteral("origin")).toString() == QStringLiteral("agent")) {
            const QJsonObject first =
                data.value(QStringLiteral("agent_messages")).toArray().first().toObject();
            sender = {{QStringLiteral("number"), first.value(QStringLiteral("from_number"))},
                      {QStringLiteral("title"), first.value(QStringLiteral("from_title"))},
                      {QStringLiteral("kind"), first.value(QStringLiteral("kind"))}};
        }
        emit notificationReceived(
            QStringLiteral("daemon/agentTurn"),
            {{QStringLiteral("threadId"), sessionId}, {QStringLiteral("sender"), sender}});
    }
    if (type == QStringLiteral("session.renamed")) {
        emit notificationReceived(
            QStringLiteral("thread/name/updated"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("threadName"), data.value(QStringLiteral("title"))},
             {QStringLiteral("customTitle"), data.value(QStringLiteral("custom_title")).toBool()}});
        return;
    }
    if (type == QStringLiteral("usage.context")) {
        emit notificationReceived(
            QStringLiteral("thread/tokenUsage/updated"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("tokenUsage"), data.value(QStringLiteral("tokenUsage"))}});
        return;
    }
    if (type == QStringLiteral("tasks.changed")) {
        emit notificationReceived(QStringLiteral("thread/backgroundTasks/updated"),
                                  {{QStringLiteral("threadId"), sessionId},
                                   {QStringLiteral("tasks"), data.value(QStringLiteral("tasks"))}});
        return;
    }
    if (type == QStringLiteral("usage.session")) {
        emit notificationReceived(QStringLiteral("thread/sessionUsage/updated"),
                                  {{"threadId", sessionId}, {"usage", data}});
        return;
    }
    if (type == QStringLiteral("usage.limits")) {
        emit notificationReceived(
            QStringLiteral("thread/planLimits/updated"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("limits"), data.value(QStringLiteral("limits"))}});
        return;
    }
    if (daemonHasTranscriptWindow_ && envelope.contains("turn_sequence")) {
        emit notificationReceived("daemon/eventScope",
                                  {{"threadId", sessionId},
                                   {"turnSequence", envelope.value("turn_sequence")},
                                   {"cursor", cursor}});
    }
    // Sidebar attention is metadata: retain it even when the transcript window is inactive.
    if (type == "approval.requested" || type == "approval.resolved" || type == "approval.expired" ||
        type == "question.answered" || type == "question.expired") {
        auto id = data.value("approval_id").toString();
        if (id.isEmpty())
            id = data.value("request_id").toString();
        emit notificationReceived("daemon/inputPending",
                                  {{"threadId", sessionId},
                                   {"requestId", id},
                                   {"pending", type == "approval.requested"}});
    }
    // Inactive conversations retain metadata, not a continuously growing copy of
    // their transcript. The next selection catches up the retained journal window.
    if (daemonHasTranscriptWindow_ &&
        (!found->windowReady || sessionId != daemonRestorePrioritySession_) &&
        (type == "assistant.delta" || type == "item.started" || type == "item.completed" ||
         type == "user.message" || type == "command.output_delta" || type == "approval.requested" ||
         type == "activity.group.summary")) {
        if (found->windowReady) {
            found->windowReady = false;
            emit daemonTranscriptInvalidated(sessionId);
        }
        return;
    }
    if (found->windowReady && sessionId == daemonRestorePrioritySession_)
        found->windowCursor = cursor;
    const QJsonObject largeEvent = data.value(QStringLiteral("large_event")).toObject();
    if (!largeEvent.isEmpty()) {
        const quint64 epoch = found->windowEpoch;
        QJsonObject event{
            {QStringLiteral("session_id"), sessionId},
            {QStringLiteral("session_generation"), generation},
            {QStringLiteral("cursor"), cursor},
            {QStringLiteral("turn_sequence"), envelope.value(QStringLiteral("turn_sequence"))},
            {QStringLiteral("event_type"), type},
            {QStringLiteral("payload"), data}};
        resolveDaemonLargeEvents(
            sessionId, generation, epoch,
            {{QStringLiteral("events"), QJsonArray{event}},
             {QStringLiteral("partial_items"), QJsonArray{}}},
            [this, sessionId, type](const QJsonObject& page, const QString& failure) {
                if (!failure.isEmpty()) {
                    emit notificationReceived(QStringLiteral("error"),
                                              {{QStringLiteral("threadId"), sessionId},
                                               {QStringLiteral("error"), failure}});
                    return;
                }
                const QJsonObject resolved = page.value(QStringLiteral("events"))
                                                 .toArray()
                                                 .first()
                                                 .toObject()
                                                 .value(QStringLiteral("payload"))
                                                 .toObject();
                const QJsonObject item = resolved.value(QStringLiteral("item")).toObject();
                if (item.isEmpty()) {
                    emit notificationReceived(
                        QStringLiteral("error"),
                        {{QStringLiteral("threadId"), sessionId},
                         {QStringLiteral("error"),
                          QStringLiteral("Reassembled transcript item is missing its item.")}});
                    return;
                }
                emit notificationReceived(
                    type == QStringLiteral("item.started") ? QStringLiteral("item/started")
                                                           : QStringLiteral("item/completed"),
                    {{QStringLiteral("threadId"), sessionId}, {QStringLiteral("item"), item}});
            });
        return;
    }
    if (type == QStringLiteral("user.message")) {
        // The session manager pairs the echo with the prompt it already shows by client message
        // id. A per-session "locally submitted" flag was cleared by unrelated events, and the
        // echo was then appended as a second copy of the same prompt.
        emit notificationReceived(
            QStringLiteral("daemon/userMessage"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), agent_message::daemonUserItemId(cursor)},
             {QStringLiteral("clientMessageId"), data.value(QStringLiteral("client_message_id"))},
             {QStringLiteral("timestamp"), envelope.value(QStringLiteral("sent_at"))},
             {QStringLiteral("text"), data.value(QStringLiteral("text"))},
             {QStringLiteral("images"), data.value(QStringLiteral("images"))},
             {QStringLiteral("agentMessages"), data.value(QStringLiteral("agent_messages"))}});
        return;
    }
    if (type == QStringLiteral("turn.steered") &&
        data.value(QStringLiteral("origin")).toString() == QStringLiteral("agent")) {
        // ADR 0027: an urgent agent message steered the running turn; show it as it arrives.
        emit notificationReceived(
            QStringLiteral("daemon/userMessage"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), QStringLiteral("steer:%1").arg(cursor)},
             {QStringLiteral("eventType"), QStringLiteral("turn.steered")},
             {QStringLiteral("text"), data.value(QStringLiteral("text"))},
             {QStringLiteral("agentMessages"), data.value(QStringLiteral("agent_messages"))}});
        return;
    }
    if (type == QStringLiteral("agent_message.state")) {
        emit notificationReceived(
            QStringLiteral("daemon/providerNotice"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), agent_message::daemonNoticeItemId(cursor)},
             {QStringLiteral("title"), agent_message::agentMessageNoticeTitle()},
             {QStringLiteral("text"), agent_message::agentMessageStateText(data)},
             {QStringLiteral("presentation"),
              QJsonObject::fromVariantMap(agent_message::agentMessageNotice(
                  data, agent_message::daemonNoticeItemId(cursor)))}});
        return;
    }
    if (type == QStringLiteral("turn.started")) {
        found->turnId = data.value(QStringLiteral("turn_id")).toString();
        emit notificationReceived(
            QStringLiteral("turn/started"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("turn"), QJsonObject{{QStringLiteral("id"), found->turnId}}}});
        return;
    }
    if (type == "activity.group.summary") {
        emit notificationReceived("daemon/activityGroup",
                                  {{"threadId", sessionId},
                                   {"turnSequence", envelope.value("turn_sequence")},
                                   {"summary", data}});
        return;
    }
    if (type == QStringLiteral("assistant.delta")) {
        emit notificationReceived(
            QStringLiteral("item/agentMessage/delta"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), data.value(QStringLiteral("item_id"))},
             {QStringLiteral("delta"), data.value(QStringLiteral("delta"))}});
        return;
    }
    if (type == QStringLiteral("command.output_delta")) {
        emit notificationReceived(
            QStringLiteral("item/commandExecution/outputDelta"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), data.value(QStringLiteral("item_id"))},
             {QStringLiteral("delta"), data.value(QStringLiteral("delta"))}});
        return;
    }
    if (type == QStringLiteral("item.started") || type == QStringLiteral("item.completed")) {
        QJsonObject item = data.value(QStringLiteral("item")).toObject();
        if (item.isEmpty()) {
            item = {{QStringLiteral("id"), data.value(QStringLiteral("item_id"))},
                    {QStringLiteral("type"), data.value(QStringLiteral("item_type"))},
                    {QStringLiteral("text"), data.value(QStringLiteral("text"))},
                    {QStringLiteral("command"), data.value(QStringLiteral("command"))},
                    {QStringLiteral("status"), data.value(QStringLiteral("status"))}};
        }
        emit notificationReceived(
            type == QStringLiteral("item.started") ? QStringLiteral("item/started")
                                                   : QStringLiteral("item/completed"),
            {{QStringLiteral("threadId"), sessionId}, {QStringLiteral("item"), item}});
        return;
    }
    if (type == QStringLiteral("turn.completed")) {
        emit notificationReceived(
            QStringLiteral("turn/completed"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("turn"),
              QJsonObject{{QStringLiteral("id"), found->turnId},
                          {QStringLiteral("status"), data.value(QStringLiteral("status"))}}}});
        emitNotice();
        found->turnId.clear();
        return;
    }
    if (type == QStringLiteral("turn.steer_failed") ||
        type == QStringLiteral("turn.interrupt_failed")) {
        // The turn is still running; record the failure without ending it.
        emit notificationReceived(
            QStringLiteral("error"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("nonFatal"), true},
             {QStringLiteral("replayed"), found->replaying},
             {QStringLiteral("error"),
              QJsonObject{{QStringLiteral("message"), data.value(QStringLiteral("message"))}}}});
        return;
    }
    if (type == QStringLiteral("runtime.reloaded"))
        return; // a planned provider reload is not a failure
    if (type == QStringLiteral("provider.request_declined")) {
        // Informational: the turn keeps running, so this must not become an "error".
        emit notificationReceived(
            QStringLiteral("daemon/providerNotice"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), agent_message::daemonNoticeItemId(cursor)},
             {QStringLiteral("text"), agent_message::providerRequestDeclinedText(data)}});
        return;
    }
    if (type == QStringLiteral("provider.protocol_error")) {
        // ADR 0015 §5: a malformed message was skipped; the turn keeps running.
        emit notificationReceived(
            QStringLiteral("daemon/providerNotice"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), agent_message::daemonNoticeItemId(cursor)},
             {QStringLiteral("title"), agent_message::providerProtocolErrorTitle()},
             {QStringLiteral("text"), agent_message::providerProtocolErrorText(data)}});
        return;
    }
    if (type == QStringLiteral("provider.runtime")) {
        // ADR 0015: the CLI runs, but its version is untested or could not be read.
        emit notificationReceived(
            QStringLiteral("daemon/providerNotice"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), agent_message::daemonNoticeItemId(cursor)},
             {QStringLiteral("title"), agent_message::providerRuntimeNoticeTitle()},
             {QStringLiteral("text"), data.value(QStringLiteral("message"))}});
        return;
    }
    if (type == QStringLiteral("runtime.error") || type == QStringLiteral("runtime.stopped")) {
        if (data.value("will_retry").toBool() || data.value("willRetry").toBool())
            return;
        emit notificationReceived(
            QStringLiteral("error"),
            {{QStringLiteral("threadId"), sessionId},
             // Replayed history must not raise old errors again as new failures.
             {QStringLiteral("replayed"), found->replaying},
             {QStringLiteral("error"),
              QJsonObject{{QStringLiteral("code"), data.value(QStringLiteral("code"))},
                          {QStringLiteral("message"), data.value(QStringLiteral("message"))}}}});
        return;
    }
    if (type == QStringLiteral("approval.requested")) {
        dispatchDaemonApproval(sessionId, data);
    }
    if (type == "approval.resolved" || type == "approval.expired" || type == "question.answered" ||
        type == "question.expired") {
        QString approvalId = data.value("approval_id").toString();
        if (approvalId.isEmpty())
            approvalId = data.value("request_id").toString();
        if (found->approvalSyncInFlight)
            found->settledApprovals.insert(approvalId);
        daemonApprovals_.remove(approvalId);
        emit notificationReceived(
            QStringLiteral("userInput/resolved"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("requestId"), approvalId},
             {QStringLiteral("expired"), type.endsWith(QStringLiteral("expired"))}});
        emit notificationReceived("daemon/approvalEnded",
                                  {{"threadId", sessionId}, {"approvalId", approvalId}});
    }
}

void CodexAppServer::dispatchDaemonApproval(const QString& sessionId, const QJsonObject& data) {
    const auto found = daemonSessions_.constFind(sessionId);
    if (found == daemonSessions_.cend())
        return;
    if (daemonHasTranscriptWindow_ &&
        (!found->windowReady || sessionId != daemonRestorePrioritySession_))
        return;
    const QString approvalId = data.value(QStringLiteral("approval_id")).toString();
    if (found->settledApprovals.contains(approvalId))
        return;
    if (approvalId.isEmpty() || daemonApprovals_.contains(approvalId))
        return;
    const QString kind = data.value(QStringLiteral("kind")).toString();
    QJsonArray questions = data.value(QStringLiteral("questions")).toArray();
    if (kind != QStringLiteral("input")) {
        QString question = data.value(QStringLiteral("reason")).toString();
        const QString command = data.value(QStringLiteral("command")).toString();
        if (!command.isEmpty()) {
            question =
                question.isEmpty() ? command : QStringLiteral("%1\n\n%2").arg(question, command);
        }
        questions = QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("decision")},
            {QStringLiteral("header"), QStringLiteral("Approval")},
            {QStringLiteral("question"),
             question.isEmpty() ? QStringLiteral("Allow this Codex action?") : question},
            {QStringLiteral("options"),
             QJsonArray{
                 QJsonObject{{QStringLiteral("label"), QStringLiteral("Accept")},
                             {QStringLiteral("description"), QStringLiteral("Allow this action")}},
                 QJsonObject{
                     {QStringLiteral("label"), QStringLiteral("Decline")},
                     {QStringLiteral("description"), QStringLiteral("Block this action")}}}}}};
    }
    daemonApprovals_.insert(
        approvalId, {sessionId, kind, data.value(QStringLiteral("revision")).toInt(1), questions});
    emit userInputRequested(
        approvalId,
        {{QStringLiteral("threadId"), sessionId},
         {QStringLiteral("turnId"), found->turnId},
         {QStringLiteral("itemId"), QStringLiteral("approval:%1").arg(approvalId)},
         {QStringLiteral("approvalKind"),
          kind == QStringLiteral("input") ? QString{} : QStringLiteral("approval")},
         {QStringLiteral("approvalReason"), data.value(QStringLiteral("reason"))},
         {QStringLiteral("approvalCommand"), data.value(QStringLiteral("command"))},
         {QStringLiteral("isBlocking"), data.value(QStringLiteral("is_blocking")).toBool(true)},
         {QStringLiteral("questions"), questions}});
}

} // namespace ketplus
