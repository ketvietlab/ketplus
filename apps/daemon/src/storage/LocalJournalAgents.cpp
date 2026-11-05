#include "storage/LocalJournal.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {
namespace {

QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }

// A null QString binds as SQL NULL, which the NOT NULL text columns reject.
QVariant textValue(const QString& value) {
    return value.isNull() ? QVariant(QStringLiteral("")) : QVariant(value);
}

QString quarantineCutoff() {
    return QDateTime::currentDateTimeUtc()
        .addDays(-LocalJournal::conversationNumberQuarantineDays)
        .toString(Qt::ISODateWithMs);
}

constexpr auto messageColumns =
    "id, chain_id, hop, from_session, from_generation, from_number, from_title, from_provider, "
    "to_session, to_generation, to_number, kind, reply_to, expect_reply, urgent, body, state, "
    "delivery, delivered_turn, error, created_at, delivered_at, expires_at";

AgentMessageRecord readMessage(const QSqlQuery& query) {
    AgentMessageRecord message;
    message.id = query.value(0).toString();
    message.chainId = query.value(1).toString();
    message.hop = query.value(2).toInt();
    message.fromSession = query.value(3).toString();
    message.fromGeneration = query.value(4).toLongLong();
    message.fromNumber = query.value(5).toString();
    message.fromTitle = query.value(6).toString();
    message.fromProvider = query.value(7).toString();
    message.toSession = query.value(8).toString();
    message.toGeneration = query.value(9).toLongLong();
    message.toNumber = query.value(10).toString();
    message.kind = query.value(11).toString();
    message.replyTo = query.value(12).toString();
    message.expectReply = query.value(13).toBool();
    message.urgent = query.value(14).toBool();
    message.body = query.value(15).toString();
    message.state = query.value(16).toString();
    message.delivery = query.value(17).toString();
    message.deliveredTurn = query.value(18).toLongLong();
    message.error = query.value(19).toString();
    message.createdAt = query.value(20).toString();
    message.deliveredAt = query.value(21).toString();
    message.expiresAt = query.value(22).toString();
    return message;
}

QList<AgentMessageRecord> readMessages(QSqlQuery& query) {
    QList<AgentMessageRecord> messages;
    while (query.next())
        messages.append(readMessage(query));
    return messages;
}

} // namespace

bool LocalJournal::initializeAgents() {
    QSqlQuery query(database_);
    const QStringList statements{
        QStringLiteral("CREATE TABLE IF NOT EXISTS conversation_numbers (number TEXT PRIMARY KEY, "
                       "session_id TEXT NOT NULL, generation INTEGER NOT NULL, "
                       "assigned_at TEXT NOT NULL, released_at TEXT NOT NULL DEFAULT '')"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS conversation_numbers_session ON "
                       "conversation_numbers(session_id, generation)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS agent_messages (id TEXT PRIMARY KEY, "
            "chain_id TEXT NOT NULL, hop INTEGER NOT NULL, from_session TEXT NOT NULL, "
            "from_generation INTEGER NOT NULL, from_number TEXT NOT NULL, "
            "from_title TEXT NOT NULL DEFAULT '', from_provider TEXT NOT NULL DEFAULT '', "
            "to_session TEXT NOT NULL, to_generation INTEGER NOT NULL, to_number TEXT NOT NULL, "
            "kind TEXT NOT NULL, reply_to TEXT NOT NULL DEFAULT '', "
            "expect_reply INTEGER NOT NULL DEFAULT 0, urgent INTEGER NOT NULL DEFAULT 0, "
            "body TEXT NOT NULL, state TEXT NOT NULL, delivery TEXT NOT NULL DEFAULT '', "
            "delivered_turn INTEGER NOT NULL DEFAULT 0, error TEXT NOT NULL DEFAULT '', "
            "created_at TEXT NOT NULL, delivered_at TEXT NOT NULL DEFAULT '', "
            "expires_at TEXT NOT NULL DEFAULT '')"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS agent_messages_receiver ON "
                       "agent_messages(to_session, to_generation, state, created_at)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS agent_messages_sender ON "
                       "agent_messages(from_session, from_generation, created_at)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS agent_messages_chain ON agent_messages(chain_id)")};
    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            setError(query.lastError().text());
            return false;
        }
    }
    return true;
}

QString LocalJournal::conversationNumber(const QString& sessionId, const qint64 generation) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT number FROM conversation_numbers WHERE session_id = ? "
                                 "AND generation = ? AND released_at = ''"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    return query.exec() && query.next() ? query.value(0).toString() : QString{};
}

QString LocalJournal::assignConversationNumber(const QString& sessionId, const qint64 generation,
                                               QString* error) {
    if (const QString existing = conversationNumber(sessionId, generation); !existing.isEmpty())
        return existing;
    const auto fail = [&](const QString& message) {
        if (error)
            *error = message;
        return QString{};
    };
    QSqlQuery used(database_);
    used.prepare(QStringLiteral("SELECT COUNT(*) FROM conversation_numbers WHERE "
                                "LENGTH(number) = 4 AND (released_at = '' OR released_at > ?)"));
    used.addBindValue(quarantineCutoff());
    if (!used.exec() || !used.next())
        return fail(used.lastError().text());
    // Random rather than sequential numbers: a mistyped number usually names no conversation.
    // Five digits once half of the four-digit range is held; assigned numbers never change.
    const bool fourDigits = used.value(0).toLongLong() < 4500;
    const quint32 low = fourDigits ? 1000 : 10000;
    const quint32 high = fourDigits ? 9999 : 99999;
    const QString cutoff = quarantineCutoff();
    // 1 assigned, 0 taken or quarantined, -1 journal error.
    const auto tryNumber = [&](const quint32 value) -> int {
        const QString number = QString::number(value);
        QSqlQuery row(database_);
        row.prepare(
            QStringLiteral("SELECT released_at FROM conversation_numbers WHERE number = ?"));
        row.addBindValue(number);
        if (!row.exec())
            return -1;
        const bool exists = row.next();
        if (exists) {
            const QString released = row.value(0).toString();
            if (released.isEmpty() || released > cutoff)
                return 0;
        }
        QSqlQuery write(database_);
        write.prepare(exists ? QStringLiteral("UPDATE conversation_numbers SET session_id = ?, "
                                              "generation = ?, assigned_at = ?, released_at = '' "
                                              "WHERE number = ?")
                             : QStringLiteral("INSERT INTO conversation_numbers(session_id, "
                                              "generation, assigned_at, number) "
                                              "VALUES(?, ?, ?, ?)"));
        write.addBindValue(sessionId);
        write.addBindValue(generation);
        write.addBindValue(now());
        write.addBindValue(number);
        return write.exec() ? 1 : -1;
    };
    auto* random = QRandomGenerator::system();
    for (int attempt = 0; attempt < 64; ++attempt) {
        const int result = tryNumber(random->bounded(low, high + 1));
        if (result < 0)
            return fail(database_.lastError().text());
        if (result > 0)
            return conversationNumber(sessionId, generation);
    }
    const quint32 start = random->bounded(low, high + 1);
    for (quint32 offset = 0; offset <= high - low; ++offset) {
        const int result = tryNumber(low + (start - low + offset) % (high - low + 1));
        if (result < 0)
            return fail(database_.lastError().text());
        if (result > 0)
            return conversationNumber(sessionId, generation);
    }
    return fail(QStringLiteral("No conversation number is free."));
}

std::optional<std::pair<QString, qint64>>
LocalJournal::sessionForConversationNumber(const QString& number) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT session_id, generation FROM conversation_numbers WHERE "
                                 "number = ? AND released_at = ''"));
    query.addBindValue(number);
    if (!query.exec() || !query.next())
        return std::nullopt;
    return std::make_pair(query.value(0).toString(), query.value(1).toLongLong());
}

QHash<QString, QString> LocalJournal::conversationNumbers() const {
    QHash<QString, QString> numbers;
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("SELECT n.session_id, n.number FROM conversation_numbers n "
                                   "JOIN sessions s ON s.id = n.session_id AND "
                                   "s.generation = n.generation WHERE n.released_at = ''")))
        return numbers;
    while (query.next())
        numbers.insert(query.value(0).toString(), query.value(1).toString());
    return numbers;
}

bool LocalJournal::insertAgentMessage(const AgentMessageRecord& message, QString* error) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("INSERT INTO agent_messages(%1) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, "
                                 "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
                      .arg(QLatin1String(messageColumns)));
    for (const QVariant& value : {textValue(message.id),
                                  textValue(message.chainId),
                                  QVariant(message.hop),
                                  textValue(message.fromSession),
                                  QVariant(message.fromGeneration),
                                  textValue(message.fromNumber),
                                  textValue(message.fromTitle),
                                  textValue(message.fromProvider),
                                  textValue(message.toSession),
                                  QVariant(message.toGeneration),
                                  textValue(message.toNumber),
                                  textValue(message.kind),
                                  textValue(message.replyTo),
                                  QVariant(message.expectReply ? 1 : 0),
                                  QVariant(message.urgent ? 1 : 0),
                                  textValue(message.body),
                                  textValue(message.state),
                                  textValue(message.delivery),
                                  QVariant(message.deliveredTurn),
                                  textValue(message.error),
                                  textValue(message.createdAt),
                                  textValue(message.deliveredAt),
                                  textValue(message.expiresAt)})
        query.addBindValue(value);
    if (!query.exec()) {
        if (error)
            *error = query.lastError().text();
        return false;
    }
    return true;
}

bool LocalJournal::updateAgentMessage(const AgentMessageRecord& message, QString* error) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("UPDATE agent_messages SET state = ?, delivery = ?, "
                                 "delivered_turn = ?, error = ?, delivered_at = ? WHERE id = ?"));
    query.addBindValue(textValue(message.state));
    query.addBindValue(textValue(message.delivery));
    query.addBindValue(message.deliveredTurn);
    query.addBindValue(textValue(message.error));
    query.addBindValue(textValue(message.deliveredAt));
    query.addBindValue(message.id);
    if (!query.exec() || query.numRowsAffected() != 1) {
        if (error)
            *error = query.lastError().text().isEmpty()
                         ? QStringLiteral("The agent message no longer exists.")
                         : query.lastError().text();
        return false;
    }
    return true;
}

std::optional<AgentMessageRecord> LocalJournal::agentMessage(const QString& id) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT %1 FROM agent_messages WHERE id = ?")
                      .arg(QLatin1String(messageColumns)));
    query.addBindValue(id);
    if (!query.exec() || !query.next())
        return std::nullopt;
    return readMessage(query);
}

QList<AgentMessageRecord> LocalJournal::agentMessagesTo(const QString& sessionId,
                                                        const qint64 generation,
                                                        const QStringList& states) const {
    if (states.isEmpty())
        return {};
    QStringList placeholders;
    for (qsizetype index = 0; index < states.size(); ++index)
        placeholders.append(QStringLiteral("?"));
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT %1 FROM agent_messages WHERE to_session = ? AND "
                                 "to_generation = ? AND state IN (%2) ORDER BY created_at, rowid")
                      .arg(QLatin1String(messageColumns), placeholders.join(QLatin1Char(','))));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    for (const QString& state : states)
        query.addBindValue(state);
    if (!query.exec())
        return {};
    return readMessages(query);
}

QList<AgentMessageRecord> LocalJournal::agentMessagesOf(const QString& sessionId,
                                                        const qint64 generation,
                                                        const int limit) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT %1 FROM agent_messages WHERE (to_session = ? AND "
                                 "to_generation = ?) OR (from_session = ? AND from_generation = ?) "
                                 "ORDER BY created_at DESC, rowid DESC LIMIT ?")
                      .arg(QLatin1String(messageColumns)));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(limit);
    if (!query.exec())
        return {};
    return readMessages(query);
}

int LocalJournal::automaticAgentTurnsInChain(const QString& chainId) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM (SELECT DISTINCT to_session, to_generation, delivered_turn FROM "
        "agent_messages WHERE chain_id = ? AND delivery = 'auto')"));
    query.addBindValue(chainId);
    return query.exec() && query.next() ? query.value(0).toInt() : 0;
}

int LocalJournal::automaticAgentTurnsSince(const QString& sessionId, const qint64 generation,
                                           const QDateTime& since) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT COUNT(DISTINCT delivered_turn) FROM agent_messages WHERE to_session = ? AND "
        "to_generation = ? AND delivery = 'auto' AND delivered_at >= ?"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(since.toUTC().toString(Qt::ISODateWithMs));
    return query.exec() && query.next() ? query.value(0).toInt() : 0;
}

QList<AgentMessageRecord> LocalJournal::overdueAgentRequests(const QDateTime& when) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT %1 FROM agent_messages WHERE expect_reply = 1 AND "
                                 "kind = 'request' AND state IN ('queued', 'held', 'delivered') "
                                 "AND expires_at <> '' AND expires_at < ? ORDER BY created_at")
                      .arg(QLatin1String(messageColumns)));
    query.addBindValue(when.toUTC().toString(Qt::ISODateWithMs));
    if (!query.exec())
        return {};
    return readMessages(query);
}

QStringList LocalJournal::sessionsWithQueuedAgentMessages() const {
    QStringList sessions;
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral(
            "SELECT DISTINCT to_session FROM agent_messages WHERE state = 'queued'")))
        return sessions;
    while (query.next())
        sessions.append(query.value(0).toString());
    return sessions;
}

bool LocalJournal::releaseAgentState(const QString& sessionId, const qint64 generation,
                                     QString* error) {
    QSqlQuery release(database_);
    release.prepare(QStringLiteral("UPDATE conversation_numbers SET released_at = ? WHERE "
                                   "session_id = ? AND generation = ? AND released_at = ''"));
    release.addBindValue(now());
    release.addBindValue(sessionId);
    release.addBindValue(generation);
    QSqlQuery expire(database_);
    expire.prepare(QStringLiteral(
        "UPDATE agent_messages SET state = 'expired', error = 'receiver_deleted' WHERE "
        "to_session = ? AND to_generation = ? AND state IN ('queued', 'held', 'delivered')"));
    expire.addBindValue(sessionId);
    expire.addBindValue(generation);
    if (!release.exec() || !expire.exec()) {
        if (error)
            *error = release.lastError().text().isEmpty() ? expire.lastError().text()
                                                          : release.lastError().text();
        return false;
    }
    return true;
}

} // namespace ketplus::daemon
