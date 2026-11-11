#include "runtime/SessionHost.h"

#include "runtime/AwaitingReply.h"
#include "storage/LocalJournal.h"

#include <QRegularExpression>

namespace ketplus::daemon {
namespace {

QJsonObject failure(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

} // namespace

// The fallback when the agent did not call request_user_decision: the answer's last paragraph,
// outside code, is a direct question. A summary that ends with a statement asks nothing.
bool answerAsksTheUser(const QString& answer) {
    QString text = answer;
    // Fenced code says nothing about what the agent asks.
    static const QRegularExpression fences(QStringLiteral("```[\\s\\S]*?(```|$)"));
    text.remove(fences);
    const QStringList paragraphs =
        text.split(QRegularExpression(QStringLiteral("\\n\\s*\\n")), Qt::SkipEmptyParts);
    for (auto it = paragraphs.crbegin(); it != paragraphs.crend(); ++it) {
        QString last = it->trimmed();
        if (last.isEmpty())
            continue;
        // A table or a quote that closes the answer is not a question to the user.
        if (last.startsWith(QLatin1Char('|')) || last.startsWith(QLatin1Char('>')))
            return false;
        // Trailing emphasis and closing brackets around the question mark.
        static const QRegularExpression trailing(QStringLiteral("[*_`)\\]\"'”’\\s]+$"));
        last.remove(trailing);
        return last.endsWith(QLatin1Char('?')) || last.endsWith(QChar(0xFF1F));
    }
    return false;
}

bool SessionHost::endsWaitingForUser(const Session& session) {
    return session.decisionRequested || answerAsksTheUser(session.lastAnswerText);
}

void SessionHost::setAwaitingReply(Session& session, const bool awaiting) {
    if (session.stored.awaitingReply == awaiting)
        return;
    session.stored.awaitingReply = awaiting;
    // A failed write leaves the state for this run; the next turn writes it again.
    (void)journal_->setSessionAwaitingReply(session.stored.id, session.stored.generation, awaiting);
}

// The turn's events: its last answer is what the heuristic reads, and the user writing again
// or a new turn starting means the conversation no longer waits.
void SessionHost::noteAwaitingReply(Session& session, const QString& type,
                                    const QJsonObject& payload) {
    if (type == QStringLiteral("user.message") || type == QStringLiteral("turn.started")) {
        if (type == QStringLiteral("user.message")) {
            session.lastAnswerText.clear();
            session.decisionRequested = false;
        }
        setAwaitingReply(session, false);
        return;
    }
    if (type != QStringLiteral("item.completed"))
        return;
    const QJsonObject item = payload.value(QStringLiteral("item")).toObject();
    if (item.value(QStringLiteral("type")).toString() != QStringLiteral("agentMessage"))
        return;
    if (const QString text = item.value(QStringLiteral("text")).toString(); !text.isEmpty())
        session.lastAnswerText = text;
}

QJsonObject SessionHost::recordUserDecision(const QString& sessionId, const qint64 generation,
                                            const QJsonObject& payload) {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end() || it->stored.generation != generation)
        return failure(QStringLiteral("session_not_found"),
                       QStringLiteral("This conversation no longer exists."));
    if (payload.value(QStringLiteral("summary")).toString().trimmed().isEmpty())
        return failure(QStringLiteral("invalid_payload"),
                       QStringLiteral("`summary` must say what the user must decide."));
    if (it->stored.turnId.isEmpty() && !it->startingTurn)
        return failure(QStringLiteral("no_turn"),
                       QStringLiteral("Call request_user_decision during a turn."));
    it->decisionRequested = true;
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("accepted"), true},
            {QStringLiteral("note"),
             QStringLiteral("This turn will end as Waiting for you. Ask the question in your "
                            "final answer.")}};
}

// "Mark as done": the user saw the question and needs nothing more from this turn.
QJsonObject SessionHost::markSessionDone(const QJsonObject& request) {
    Session* session = nullptr;
    if (const QJsonObject failed = checkedSession(request, &session); !failed.isEmpty())
        return failed;
    setAwaitingReply(*session, false);
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("accepted"), true},
            {QStringLiteral("session_id"), session->stored.id},
            {QStringLiteral("session_generation"), session->stored.generation},
            {QStringLiteral("awaiting_reply"), false}};
}

} // namespace ketplus::daemon
