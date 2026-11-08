#include "storage/LocalJournal.h"

#include <QSqlError>
#include <limits>

namespace ketplus::daemon {

QJsonArray LocalJournal::appendTurnStart(const StoredSession& session, const QJsonObject& prompt,
                                         const QJsonObject& configuration, QString* error) {
    const qint64 count = configuration.isEmpty() ? 2 : 3;
    if (session.lastCursor < 0 || session.lastCursor > std::numeric_limits<qint64>::max() - count) {
        if (error)
            *error = QStringLiteral("Journal counters are unsupported or exhausted.");
        return {};
    }
    if (!database_.transaction()) {
        if (error)
            *error = database_.lastError().text();
        return {};
    }
    const auto fail = [&](QString message) {
        if (message.isEmpty())
            message = QStringLiteral("The local journal rejected turn acceptance.");
        database_.rollback();
        setError(message);
        if (error)
            *error = message;
        return QJsonArray{};
    };
    if (!upsertSession(session) || !recordSessionContext(session))
        return fail(errorString());
    auto contextualPrompt = prompt;
    contextualPrompt.insert(QStringLiteral("context_revision"), session.contextRevision);
    QString appendError;
    const auto first =
        appendEventWithMutation(session.id, session.generation, QStringLiteral("user.message"),
                                contextualPrompt, &appendError, {}, false);
    if (first < 0)
        return fail(appendError);
    if (appendEventWithMutation(session.id, session.generation, QStringLiteral("turn.queued"),
                                {{QStringLiteral("context_revision"), session.contextRevision}},
                                &appendError, {}, false) < 0)
        return fail(appendError);
    if (!configuration.isEmpty() &&
        appendEventWithMutation(session.id, session.generation,
                                QStringLiteral("session.configuration_changed"), configuration,
                                &appendError, {}, false) < 0)
        return fail(appendError);
    // Read the bounded receipt before commit: a read failure must not become a
    // rejected command whose prompt has already been persisted.
    const auto events = eventsAfter(session.id, session.generation, first - 1, int(count));
    if (events.size() != count)
        return fail(QStringLiteral("The turn acceptance receipt is incomplete."));
    if (!database_.commit())
        return fail(database_.lastError().text());
    return events;
}

} // namespace ketplus::daemon
