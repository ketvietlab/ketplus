#include "providers/CursorAdapter.h"

namespace ketplus::daemon {
void CursorAdapter::respond(const QJsonValue& requestId, const QJsonObject& result) {
    if (planRequests_.remove(requestKey(requestId))) {
        const bool allow = result.value("allow").toBool() || result.value("decision") == "accept";
        sendObject(
            {{"jsonrpc", "2.0"},
             {"id", requestId},
             {"result", QJsonObject{{"outcome",
                                     QJsonObject{{"outcome", allow ? "accepted" : "rejected"}}}}}});
        return;
    }
    const auto asked = questionOptionsByRequest_.take(requestKey(requestId));
    if (!asked.isEmpty() && result.value("answers").toObject().isEmpty()) {
        sendObject({{"jsonrpc", "2.0"},
                    {"id", requestId},
                    {"result", QJsonObject{{"outcome", QJsonObject{{"outcome", "cancelled"}}}}}});
        return;
    }
    if (!asked.isEmpty()) {
        QJsonArray answers;
        const QJsonObject submitted = result.value(QStringLiteral("answers")).toObject();
        for (const QJsonValue& questionValue : asked) {
            const QJsonObject question = questionValue.toObject();
            const QString questionId = question.value(QStringLiteral("id")).toString();
            const QJsonArray values =
                submitted.value(questionId).toObject().value(QStringLiteral("answers")).toArray();
            QJsonArray selected;
            for (const QJsonValue& value : values) {
                const QString label = value.toString();
                for (const QJsonValue& optionValue :
                     question.value(QStringLiteral("options")).toArray()) {
                    const QJsonObject option = optionValue.toObject();
                    if (option.value(QStringLiteral("label")).toString() == label) {
                        selected.append(option.value(QStringLiteral("id")));
                        break;
                    }
                }
            }
            answers.append(QJsonObject{{QStringLiteral("questionId"), questionId},
                                       {QStringLiteral("selectedOptionIds"), selected}});
        }
        sendObject(
            {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
             {QStringLiteral("id"), requestId},
             {QStringLiteral("result"),
              QJsonObject{{QStringLiteral("outcome"),
                           QJsonObject{{QStringLiteral("outcome"), QStringLiteral("answered")},
                                       {QStringLiteral("answers"), answers}}}}}});
        return;
    }
    AcpAdapter::respond(requestId, result);
}

void CursorAdapter::respondError(const QJsonValue& id, const QString& code,
                                 const QString& message) {
    questionOptionsByRequest_.remove(requestKey(id));
    planRequests_.remove(requestKey(id));
    AcpAdapter::respondError(id, code, message);
}

bool CursorAdapter::handleExtensionRequest(const QJsonObject& object) {
    const QString method = object.value(QStringLiteral("method")).toString();
    const QJsonObject params = object.value(QStringLiteral("params")).toObject();
    if (method == QStringLiteral("cursor/ask_question")) {
        const QString acpSessionId = params.value(QStringLiteral("sessionId")).toString();
        const QString sessionId = ketplusByAcp_.value(acpSessionId);
        QJsonArray questions;
        for (const QJsonValue& questionValue :
             params.value(QStringLiteral("questions")).toArray()) {
            const QJsonObject question = questionValue.toObject();
            questions.append(QJsonObject{
                {QStringLiteral("id"), question.value(QStringLiteral("id"))},
                {QStringLiteral("header"), params.value(QStringLiteral("title"))},
                {QStringLiteral("question"), question.value(QStringLiteral("prompt"))},
                {QStringLiteral("selection"),
                 question.value(QStringLiteral("allowMultiple")).toBool()
                     ? QStringLiteral("multiple")
                     : QStringLiteral("single")},
                {QStringLiteral("options"), question.value(QStringLiteral("options"))}});
        }
        if (sessionId.isEmpty() || questions.isEmpty()) {
            respond(object.value(QStringLiteral("id")),
                    {{QStringLiteral("outcome"),
                      QJsonObject{{QStringLiteral("outcome"), QStringLiteral("skipped")}}}});
            return true;
        }
        questionOptionsByRequest_.insert(requestKey(object.value(QStringLiteral("id"))), questions);
        QJsonObject normalized{
            {QStringLiteral("sessionId"), sessionId},
            {QStringLiteral("itemId"), params.value(QStringLiteral("toolCallId"))},
            {QStringLiteral("questions"), questions},
            {QStringLiteral("isBlocking"), true}};
        emit requestReceived(object.value(QStringLiteral("id")), method, normalized);
        return true;
    }
    if (method == QStringLiteral("cursor/create_plan")) {
        QString sessionId = ketplusByAcp_.value(params.value("sessionId").toString());
        if (sessionId.isEmpty()) {
            for (auto turn = turns_.cbegin(); turn != turns_.cend(); ++turn)
                if (turn->toolItems.contains(params.value("toolCallId").toString())) {
                    sessionId = turn.key();
                    break;
                }
        }
        if (sessionId.isEmpty()) {
            respondError(object.value("id"), "unknown_session", "Plan has no active session.");
            return true;
        }
        planRequests_.insert(requestKey(object.value("id")));
        const QJsonObject item{{"id", params.value("toolCallId")},
                               {"type", "dynamicToolCall"},
                               {"tool", "Plan"},
                               {"arguments", params},
                               {"status", "completed"}};
        emit canonicalEvent(sessionId, "item.completed",
                            {{"item_id", item.value("id")}, {"item", item}});
        emit requestReceived(
            object.value("id"), "permission.request",
            {{"sessionId", sessionId}, {"tool", "Accept plan"}, {"input", params}});
        return true;
    }
    return false;
}

bool CursorAdapter::autoApprovePermission(const QString& sessionId,
                                          const QJsonObject& toolCall) const {
    // Cursor supplies these fields as CLI-authenticated MCP identity, not model arguments.
    const auto input = toolCall.value(QStringLiteral("rawInput")).toObject();
    if (input.value(QStringLiteral("toolName")).toString().isEmpty())
        return false;
    const QString server = input.value(QStringLiteral("providerIdentifier")).toString();
    return (browserSessions_.contains(sessionId) && server == QStringLiteral("ketplus_browser")) ||
           (computerSessions_.contains(sessionId) &&
            server == QStringLiteral("ketplus_computer")) ||
           (agentSessions_.contains(sessionId) && server == QStringLiteral("ketplus_agents"));
}

} // namespace ketplus::daemon
