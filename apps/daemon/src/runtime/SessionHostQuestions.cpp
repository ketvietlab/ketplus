#include "runtime/ApprovalBroker.h"
#include "runtime/SessionHost.h"
#include <QJsonDocument>
#include <QTimer>

namespace ketplus::daemon {
void SessionHost::registerAssistantQuestions(Session& session, const QJsonObject& item) {
    if (session.stored.turnId.isEmpty() || item.value("type") != "agentMessage" ||
        item.value("delivery") != "async")
        return;
    const auto source = item.value("questions").toArray();
    const auto itemId = item.value("id").toString();
    const auto key =
        session.stored.id + ":" + QString::number(session.stored.generation) + ":" + itemId;
    if (source.isEmpty() || itemId.isEmpty() || assistantQuestions_.contains(key))
        return;
    QJsonArray questions;
    for (qsizetype index = 0; index < source.size(); ++index) {
        const auto raw = source.at(index).toObject();
        QJsonArray options;
        for (const auto& option : raw.value("options").toArray())
            options.append(option.isString() ? QJsonObject{{"label", option.toString()}}
                                             : option.toObject());
        questions.append(QJsonObject{{"id", QStringLiteral("question-%1").arg(index)},
                                     {"question", raw.value("title")},
                                     {"options", options},
                                     {"isOther", true}});
    }
    const QJsonObject tag{{"ketplus_async_question", true},
                          {"session_id", session.stored.id},
                          {"generation", session.stored.generation},
                          {"turn_id", session.stored.turnId},
                          {"turn_sequence", session.stored.currentTurnSequence},
                          {"questions", questions}};
    const auto id = approvals_->request(
        session.stored.id, session.stored.generation, tag, "input",
        {{"item_id", itemId}, {"is_blocking", false}, {"questions", questions}});
    if (!id.isEmpty())
        assistantQuestions_.insert(key);
}

bool SessionHost::answerAssistantQuestions(const QJsonValue& id, const QJsonObject& result) {
    const auto tag = id.toObject();
    if (!tag.value("ketplus_async_question").toBool())
        return false;
    const auto answers = result.value("answers").toObject();
    if (answers.isEmpty())
        return true; // Stop/expiry never starts a new turn.
    QStringList lines;
    for (const auto& value : tag.value("questions").toArray()) {
        const auto question = value.toObject();
        QStringList values;
        for (const auto& answer :
             answers.value(question.value("id").toString()).toObject().value("answers").toArray())
            values.append(answer.toString());
        lines.append(question.value("question").toString() + "\n" + values.join(", "));
    }
    const auto sessionId = tag.value("session_id").toString();
    const auto generation = tag.value("generation").toInteger();
    const auto prompt = lines.join("\n\n");
    const auto turnId = tag.value("turn_id").toString();
    const auto turnSequence = tag.value("turn_sequence").toInteger();
    // Finish approval arbitration before the resulting turn generates new events.
    QTimer::singleShot(0, this, [this, sessionId, generation, turnId, turnSequence, prompt] {
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end() || it->stored.generation != generation || turnId.isEmpty() ||
            it->stored.turnId != turnId || it->stored.currentTurnSequence != turnSequence)
            return;
        QJsonObject request{
            {"session_id", sessionId},
            {"session_generation", generation},
            {"payload", QJsonObject{{"prompt", prompt},
                                    {"expected_context_revision", it->stored.contextRevision}}}};
        // Resolve against the live context after approval arbitration, just as the
        // composer does. A switched conversation rejects unversioned input.
        const auto response = steer(request);
        if (!response.value("ok").toBool())
            appendEvent(
                *it, "runtime.error",
                {{"message", QStringLiteral("Unable to deliver the question answer: %1 (%2)")
                                 .arg(response.value("error_message").toString(),
                                      response.value("error_code").toString())}});
    });
    return true;
}
} // namespace ketplus::daemon
