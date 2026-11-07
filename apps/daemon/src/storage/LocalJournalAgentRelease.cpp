#include "storage/LocalJournal.h"

#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

namespace ketplus::daemon {

bool LocalJournal::authorizeAgentMessages(const QString& sessionId, qint64 generation,
                                          const QStringList& ids, const QString& authorizationId,
                                          QString* error) {
    const auto fail = [&](const QString& reason) {
        database_.rollback();
        if (error)
            *error = reason;
        return false;
    };
    if (ids.isEmpty() || ids.size() > 100 || authorizationId.isEmpty() ||
        authorizationId.size() > 128 ||
        QSet<QString>(ids.cbegin(), ids.cend()).size() != ids.size()) {
        if (error)
            *error = QStringLiteral("Choose between 1 and 100 distinct held messages.");
        return false;
    }
    if (!database_.transaction()) {
        if (error)
            *error = database_.lastError().text();
        return false;
    }
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto& id : ids) {
        const auto saved = agentMessage(id);
        if (!saved || saved->toSession != sessionId || saved->toGeneration != generation)
            return fail(
                QStringLiteral("The selected message does not belong to this conversation."));
        const bool retry =
            saved->state == "failed" && saved->delivery.startsWith("user_authorized:");
        if (saved->state != "held" && !retry)
            return fail(
                QStringLiteral("The selected message is no longer held. Refresh its status."));
        if (!saved->expiresAt.isEmpty() &&
            QDateTime::fromString(saved->expiresAt, Qt::ISODateWithMs) <= now)
            return fail(QStringLiteral("The selected message has expired."));
        auto message = *saved;
        message.state = QStringLiteral("queued");
        message.delivery = QStringLiteral("user_authorized:") + authorizationId;
        message.error.clear();
        message.deliveredTurn = 0;
        message.deliveredAt.clear();
        QString updateError;
        if (!updateAgentMessage(message, &updateError))
            return fail(updateError);
    }
    if (!database_.commit())
        return fail(database_.lastError().text());
    return true;
}

bool LocalJournal::claimAgentRelease(const QList<AgentMessageRecord>& messages, qint64 turnSequence,
                                     QString* error) {
    if (messages.isEmpty() || turnSequence < 1 || !database_.transaction()) {
        if (error)
            *error = QStringLiteral("Unable to claim the authorized batch.");
        return false;
    }
    for (auto message : messages) {
        const auto current = agentMessage(message.id);
        if (!current || current->state != "queued" ||
            !current->delivery.startsWith("user_authorized:") ||
            current->delivery != message.delivery) {
            database_.rollback();
            if (error)
                *error = QStringLiteral("The authorized batch changed before delivery.");
            return false;
        }
        message.state = QStringLiteral("releasing");
        message.deliveredTurn = turnSequence;
        if (!updateAgentMessage(message, error)) {
            database_.rollback();
            return false;
        }
    }
    if (!database_.commit()) {
        if (error)
            *error = database_.lastError().text();
        database_.rollback();
        return false;
    }
    return true;
}

std::optional<bool> LocalJournal::agentReleaseJournaled(const AgentMessageRecord& message) const {
    if (!message.delivery.startsWith("user_authorized:") || message.deliveredTurn < 1)
        return false;
    QSqlQuery query(database_);
    // The compacted transcript retains user.message after raw-event pruning.
    query.prepare(QStringLiteral(
        "SELECT payload FROM events WHERE session_id=? AND generation=? AND turn_sequence=? "
        "AND event_type='user.message' UNION ALL SELECT payload FROM replay_events "
        "WHERE session_id=? AND generation=? AND turn_sequence=? AND event_type='user.message'"));
    for (int i = 0; i < 2; ++i) {
        query.addBindValue(message.toSession);
        query.addBindValue(message.toGeneration);
        query.addBindValue(message.deliveredTurn);
    }
    if (!query.exec())
        return std::nullopt;
    const auto authorizationId = message.delivery.mid(QStringLiteral("user_authorized:").size());
    while (query.next()) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(query.value(0).toByteArray(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            return std::nullopt;
        const auto payload = document.object();
        if (payload.value("user_release_id").toString() == authorizationId)
            return true;
    }
    return false;
}

} // namespace ketplus::daemon
