#include "BrowserAgentInstructions.h"
#include "BrowserToolLaunch.h"
#include "ComputerAgentInstructions.h"
#include "providers/acp/AcpAdapter.h"
#include "providers/acp/AcpToolItem.h"
#include "runtime/ResponseGuidance.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>
#include <algorithm>
#include <utility>
namespace {
constexpr qsizetype maximumImageBytes = 4 * 1024 * 1024;
QJsonObject occupancyTokenUsage(const QJsonObject& source) {
    QJsonObject usage = source.value(QStringLiteral("tokenUsage")).toObject();
    if (usage.isEmpty())
        usage = source.value(QStringLiteral("usage")).toObject();
    const QJsonObject last = usage.value(QStringLiteral("last")).toObject();
    qint64 used = last.value(QStringLiteral("totalTokens"))
                      .toInteger(last.value(QStringLiteral("total_tokens")).toInteger(-1));
    qint64 limit = usage.value(QStringLiteral("modelContextWindow"))
                       .toInteger(usage.value(QStringLiteral("model_context_window")).toInteger(0));
    if (used < 0) {
        used = source.value(QStringLiteral("used"))
                   .toInteger(usage.value(QStringLiteral("used")).toInteger(-1));
    }
    if (limit <= 0) {
        limit = source.value(QStringLiteral("size"))
                    .toInteger(usage.value(QStringLiteral("size")).toInteger(0));
    }
    if (used < 0 || limit <= 0)
        return {};
    return {{QStringLiteral("last"), QJsonObject{{QStringLiteral("totalTokens"), used}}},
            {QStringLiteral("modelContextWindow"), limit}};
}

QString mimeForPath(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg"))
        return QStringLiteral("image/jpeg");
    return QStringLiteral("image/png");
}

} // namespace

namespace ketplus::daemon {
void AcpAdapter::call(const QString& method, const QJsonObject& params, Callback callback) {
    if (!isAvailable()) {
        callback({}, missingMessage());
        return;
    }
    if (!ready_) {
        queued_.append({method, params, std::move(callback)});
        ensureStarted();
        return;
    }
    if (method == QStringLiteral("turn/start"))
        startTurn(params, std::move(callback));
    else if (method == QStringLiteral("turn/interrupt"))
        interruptTurn(params, std::move(callback));
    else
        sendCall(method, params, std::move(callback));
}

void AcpAdapter::startTurn(const QJsonObject& params, Callback callback) {
    const QString sessionId = params.value(QStringLiteral("sessionId")).toString();
    const QString cwd = params.value(QStringLiteral("cwd")).toString();
    if (sessionId.isEmpty() || cwd.isEmpty()) {
        callback({}, QStringLiteral("%1 turn is missing a session or worktree.").arg(label_));
        return;
    }
    // One turn at a time per session: a second start would drop the running turn's state, and its
    // still-pending prompt reply would then complete the new turn.
    if (turns_.contains(sessionId) || startingSessions_.contains(sessionId)) {
        callback({}, QStringLiteral("A %1 turn is already running for this session.").arg(label_));
        return;
    }
    startingSessions_.insert(sessionId);
    permissionBySession_.insert(sessionId, params.value(QStringLiteral("permission")).toString());
    if (const QJsonObject server = params.value(QStringLiteral("agentMcpServer")).toObject();
        server.isEmpty()) {
        agentServers_.remove(sessionId);
        agentInstructions_.remove(sessionId);
    } else {
        agentServers_.insert(sessionId, server);
        agentInstructions_.insert(sessionId,
                                  params.value(QStringLiteral("agentInstructions")).toString());
    }
    ensureAcpSession(
        sessionId, cwd, params.value(QStringLiteral("resume")).toString(),
        params.value(QStringLiteral("model")).toString(),
        [this, params, callback = std::move(callback), sessionId](const QJsonObject& result,
                                                                  const QString& error) {
            startingSessions_.remove(sessionId);
            if (!error.isEmpty()) {
                callback({}, sessionError(error));
                return;
            }
            TurnState turn;
            turn.ketplusSessionId = sessionId;
            turn.acpSessionId = result.value(QStringLiteral("sessionId")).toString();
            turn.turnId = QStringLiteral("%1-turn-%2").arg(name()).arg(nextTurnNumber_++);
            turn.itemId = QStringLiteral("%1-answer-%2").arg(name(), turn.turnId);
            turn.permission = permissionBySession_.value(sessionId);
            turns_.insert(sessionId, turn);
            callback(QJsonObject{{QStringLiteral("turn"),
                                  QJsonObject{{QStringLiteral("id"), turn.turnId}}},
                                 {QStringLiteral("thread"),
                                  QJsonObject{{QStringLiteral("id"), turn.acpSessionId}}}},
                     {});
            sendPrompt(turns_[sessionId], params);
        });
}

void AcpAdapter::sendPrompt(TurnState& turn, const QJsonObject& params) {
    // ACP has no system-prompt field. Supply host context as a separate block;
    // the journal and user prompt remain unchanged, including image attachments.
    auto prompt = promptContent(params);
    QString hostText =
        browser_tools::hostAgentInstructions(browserSessions_.contains(turn.ketplusSessionId)) +
        QStringLiteral("\n\n") +
        computer_tools::agentInstructions(computerSessions_.contains(turn.ketplusSessionId));
    const bool agentTools = agentSessions_.contains(turn.ketplusSessionId);
    if (agentTools && !agentInstructions_.value(turn.ketplusSessionId).isEmpty())
        hostText += QStringLiteral("\n\n") + agentInstructions_.value(turn.ketplusSessionId);
    hostText = withResponseGuidance(hostText, agentTools);
    prompt.prepend(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                               {QStringLiteral("text"), hostText}});
    emit canonicalEvent(turn.ketplusSessionId, QStringLiteral("turn.started"),
                        {{QStringLiteral("turn_id"), turn.turnId},
                         {QStringLiteral("thread_id"), turn.acpSessionId}});
    const quint64 id = nextRequestId_++;
    pending_.insert(id, [this, sessionId = turn.ketplusSessionId](const QJsonObject& result,
                                                                  const QString& error) {
        auto it = turns_.find(sessionId);
        if (it == turns_.end())
            return;
        if (!error.isEmpty()) {
            finishTurn(sessionId, QStringLiteral("failed"), turnFailureMessage(error));
            return;
        }
        if (isFailureText(it->assistantText)) {
            finishTurn(sessionId, QStringLiteral("failed"), turnFailureMessage(it->assistantText));
            return;
        }
        emitContextUsage(sessionId, result);
        const QString stop = result.value(QStringLiteral("stopReason")).toString();
        finishTurn(sessionId,
                   stop == QStringLiteral("cancelled") || stop == QStringLiteral("interrupted")
                       ? QStringLiteral("interrupted")
                       : QStringLiteral("completed"));
    });
    sendObject(
        {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
         {QStringLiteral("id"), static_cast<qint64>(id)},
         {QStringLiteral("method"), QStringLiteral("session/prompt")},
         {QStringLiteral("params"), QJsonObject{{QStringLiteral("sessionId"), turn.acpSessionId},
                                                {QStringLiteral("prompt"), prompt}}}});
}

QJsonArray AcpAdapter::promptContent(const QJsonObject& params) const {
    QJsonArray prompt;
    const QString text = params.value(QStringLiteral("prompt")).toString();
    if (!text.isEmpty())
        prompt.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                  {QStringLiteral("text"), text}});
    for (const QJsonValue& value : params.value(QStringLiteral("images")).toArray()) {
        const QString path = value.toString();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray bytes = file.read(maximumImageBytes + 1);
        if (bytes.isEmpty() || bytes.size() > maximumImageBytes)
            continue;
        prompt.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                                  {QStringLiteral("mimeType"), mimeForPath(path)},
                                  {QStringLiteral("data"), QString::fromLatin1(bytes.toBase64())}});
    }
    if (prompt.isEmpty())
        prompt.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                  {QStringLiteral("text"), QStringLiteral(" ")}});
    return prompt;
}

void AcpAdapter::interruptTurn(const QJsonObject& params, Callback callback) {
    const QString sessionId = params.value(QStringLiteral("sessionId")).toString();
    const QString acpId = acpSessionByKetplus_.value(sessionId);
    if (acpId.isEmpty()) {
        callback(
            {},
            QStringLiteral("The %1 session has no active ACP session to interrupt.").arg(label_));
        return;
    }
    auto turn = turns_.find(sessionId);
    if (turn != turns_.end())
        turn->interrupted = true;
    sendObject({{"jsonrpc", "2.0"},
                {"method", "session/cancel"},
                {"params", QJsonObject{{"sessionId", acpId}}}});
    callback({}, {});
}

void AcpAdapter::finishTurn(const QString& ketplusSessionId, const QString& status,
                            const QString& errorMessage) {
    const auto it = turns_.find(ketplusSessionId);
    if (it == turns_.end())
        return;
    const TurnState turn = it.value();
    turns_.erase(it);
    if (const QString signal = browserSessions_.value(ketplusSessionId);
        !signal.isEmpty() && !browser_tools::signalTurnEnd(signal))
        qWarning("Unable to signal the browser server that a turn of %s ended",
                 qUtf8Printable(ketplusSessionId));
    if (!turn.assistantText.isEmpty() && !isFailureText(turn.assistantText)) {
        emit canonicalEvent(
            ketplusSessionId, QStringLiteral("item.completed"),
            {{QStringLiteral("item_id"), turn.itemId},
             {QStringLiteral("item_type"), QStringLiteral("agentMessage")},
             {QStringLiteral("text"), turn.assistantText},
             {QStringLiteral("item"),
              QJsonObject{{QStringLiteral("id"), turn.itemId},
                          {QStringLiteral("type"), QStringLiteral("agentMessage")},
                          {QStringLiteral("phase"), QStringLiteral("final_answer")},
                          {QStringLiteral("text"), turn.assistantText},
                          {QStringLiteral("status"), QStringLiteral("completed")}}}});
    }
    if (!turn.interrupted && !errorMessage.isEmpty()) {
        QJsonObject payload{{QStringLiteral("message"), errorMessage}};
        if (const qint64 resetsAt = failureResetsAt(errorMessage); resetsAt > 0)
            payload.insert(QStringLiteral("resets_at"), resetsAt);
        emit canonicalEvent(ketplusSessionId, QStringLiteral("runtime.error"), payload);
    }
    emit canonicalEvent(
        ketplusSessionId, QStringLiteral("turn.completed"),
        {{QStringLiteral("status"), turn.interrupted ? QStringLiteral("interrupted") : status},
         {QStringLiteral("thread_id"), turn.acpSessionId}});
}

void AcpAdapter::emitContextUsage(const QString& sessionId, const QJsonObject& source) {
    const QJsonObject usage = occupancyTokenUsage(source);
    if (sessionId.isEmpty() || usage.isEmpty())
        return;
    emit canonicalEvent(sessionId, QStringLiteral("usage.context"),
                        {{QStringLiteral("tokenUsage"), usage}});
}

void AcpAdapter::handleSessionUpdate(const QJsonObject& params) {
    QJsonObject update = params.value(QStringLiteral("update")).toObject();
    if (update.isEmpty() && params.contains(QStringLiteral("sessionUpdate")))
        update = params;
    const QString acpId = params.value(QStringLiteral("sessionId")).toString();
    const QString sessionId = ketplusByAcp_.value(acpId);
    if (sessionId.isEmpty())
        return;
    const QString kind = update.value(QStringLiteral("sessionUpdate")).toString();
    if (kind == QStringLiteral("usage_update")) {
        emitContextUsage(sessionId, update);
        return;
    }
    auto it = turns_.find(sessionId);
    if (it == turns_.end())
        return;
    if (kind == QStringLiteral("agent_message_chunk")) {
        const QString delta = update.value(QStringLiteral("content"))
                                  .toObject()
                                  .value(QStringLiteral("text"))
                                  .toString();
        if (delta.isEmpty())
            return;
        const QString combined = it->assistantText + delta;
        if (holdAssistantText(combined)) {
            it->assistantText = combined;
            it->heldAssistant = true;
            return;
        }
        QString outgoing = delta;
        if (it->heldAssistant) {
            outgoing = combined;
            it->assistantText.clear();
            it->heldAssistant = false;
        }
        if (outgoing.isEmpty())
            return;
        if (it->assistantText.isEmpty()) {
            emit canonicalEvent(
                sessionId, QStringLiteral("item.started"),
                {{QStringLiteral("item_id"), it->itemId},
                 {QStringLiteral("item_type"), QStringLiteral("agentMessage")},
                 {QStringLiteral("item"),
                  QJsonObject{{QStringLiteral("id"), it->itemId},
                              {QStringLiteral("type"), QStringLiteral("agentMessage")},
                              {QStringLiteral("phase"), QStringLiteral("final_answer")},
                              {QStringLiteral("text"), QString()},
                              {QStringLiteral("status"), QStringLiteral("inProgress")}}}});
        }
        it->assistantText += outgoing;
        emit canonicalEvent(
            sessionId, QStringLiteral("assistant.delta"),
            {{QStringLiteral("item_id"), it->itemId}, {QStringLiteral("delta"), outgoing}});
        return;
    }
    if (kind == QStringLiteral("tool_call") || kind == QStringLiteral("tool_call_update")) {
        const QString toolId = update.value("toolCallId").toString();
        if (toolId.isEmpty())
            return;
        auto& raw = it->toolItems[toolId];
        raw = acpToolUpdate(raw, update);
        const auto item = acpToolItem(raw);
        const auto status = item.value("status").toString();
        emit canonicalEvent(
            sessionId, status == "inProgress" ? "item.started" : "item.completed",
            {{"item_id", toolId}, {"item_type", item.value("type")}, {"item", item}});
        return;
    }
    if (kind == "plan") {
        const QJsonObject item{{"id", it->turnId + ":plan"},
                               {"type", "dynamicToolCall"},
                               {"tool", "Plan"},
                               {"status", "completed"},
                               {"entries", update.value("entries")}};
        emit canonicalEvent(sessionId, "item.completed",
                            {{"item_id", item.value("id")}, {"item", item}});
    }
}

} // namespace ketplus::daemon
