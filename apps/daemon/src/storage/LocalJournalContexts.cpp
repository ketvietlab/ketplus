#include "storage/LocalJournal.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {

bool LocalJournal::initializeContexts() {
    QSqlQuery query(database_);
    if (!query.exec(
            QStringLiteral("CREATE TABLE IF NOT EXISTS session_contexts (session_id TEXT NOT NULL, "
                           "generation INTEGER NOT NULL, context_revision INTEGER NOT NULL, "
                           "worktree_path TEXT NOT NULL, first_cursor INTEGER NOT NULL, "
                           "PRIMARY KEY(session_id, generation, context_revision))"))) {
        setError(query.lastError().text());
        return false;
    }
    if (!query.exec(
            "CREATE TABLE IF NOT EXISTS conversation_worktree_activity (session_id TEXT NOT NULL, "
            "generation INTEGER NOT NULL, worktree_path TEXT NOT NULL, last_cursor INTEGER NOT "
            "NULL, PRIMARY KEY(session_id,generation,worktree_path))")) {
        setError(query.lastError().text());
        return false;
    }
    if (!query.exec("CREATE TABLE IF NOT EXISTS conversation_navigation (session_id TEXT NOT NULL, "
                    "generation INTEGER NOT NULL, path TEXT NOT NULL, PRIMARY "
                    "KEY(session_id,generation))")) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

QString LocalJournal::conversationPath(const StoredSession& session) const {
    QSqlQuery q(database_);
    q.prepare("SELECT path FROM conversation_navigation WHERE session_id=? AND generation=?");
    q.addBindValue(session.id);
    q.addBindValue(session.generation);
    return q.exec() && q.next() ? q.value(0).toString() : session.worktreePath;
}
bool LocalJournal::recordConversationPath(const StoredSession& session) {
    QSqlQuery q(database_);
    q.prepare("INSERT OR IGNORE INTO conversation_navigation VALUES (?,?,?)");
    q.addBindValue(session.id);
    q.addBindValue(session.generation);
    q.addBindValue(session.worktreePath);
    return q.exec();
}

bool LocalJournal::rememberWorktree(const QString& path) {
    QSqlQuery q(database_);
    q.prepare(
        "INSERT OR IGNORE INTO "
        "worktree_catalog(path,catalog_revision,lifecycle_revision,updated_at) VALUES (?,0,0,?)");
    q.addBindValue(path);
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    return q.exec();
}

bool LocalJournal::recordWorktreeActivity(const StoredSession& session, const QString& path,
                                          qint64 cursor) {
    QSqlQuery q(database_);
    q.prepare("INSERT INTO conversation_worktree_activity VALUES (?,?,?,?) ON "
              "CONFLICT(session_id,generation,worktree_path) DO UPDATE SET "
              "last_cursor=MAX(last_cursor,excluded.last_cursor)");
    q.addBindValue(session.id);
    q.addBindValue(session.generation);
    q.addBindValue(path);
    q.addBindValue(cursor);
    return q.exec() && rememberWorktree(path);
}

QSet<QString> LocalJournal::touchedWorktreePaths(const QString& id, qint64 generation) const {
    QSqlQuery q(database_);
    q.prepare("SELECT worktree_path FROM conversation_worktree_activity WHERE session_id=? AND "
              "generation=?");
    q.addBindValue(id);
    q.addBindValue(generation);
    QSet<QString> paths;
    if (q.exec())
        while (q.next())
            paths.insert(q.value(0).toString());
    return paths;
}

qint64 LocalJournal::turnContextRevision(const QString& sessionId, qint64 generation,
                                         const QString& turnId) const {
    if (turnId.isEmpty())
        return 0;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT json_extract(payload, '$.context_revision') FROM events "
        "WHERE session_id = ? AND generation = ? AND event_type = 'turn.started' "
        "AND json_extract(payload, '$.turn_id') = ? "
        "AND json_extract(payload, '$.provenance') = 'turn_context' ORDER BY cursor LIMIT 1"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(turnId);
    return query.exec() && query.next() ? query.value(0).toLongLong() : 0;
}

bool LocalJournal::recordSessionContext(const StoredSession& session) {
    if (!recordConversationPath(session))
        return false;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO session_contexts VALUES (?, ?, ?, ?, ?)"));
    query.addBindValue(session.id);
    query.addBindValue(session.generation);
    query.addBindValue(session.contextRevision);
    query.addBindValue(session.worktreePath);
    query.addBindValue(session.lastCursor + 1);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

QJsonArray LocalJournal::sessionContexts(const QString& sessionId, qint64 generation) const {
    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral("SELECT context_revision, worktree_path, first_cursor FROM session_contexts "
                       "WHERE session_id = ? AND generation = ? ORDER BY context_revision"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec()) {
        setError(query.lastError().text());
        return {};
    }
    QJsonArray result;
    while (query.next()) {
        result.append(QJsonObject{{"context_revision", query.value(0).toLongLong()},
                                  {"worktree_path", query.value(1).toString()},
                                  {"first_event_cursor", query.value(2).toLongLong()}});
    }
    return result;
}

qint64 LocalJournal::switchSessionWorktree(const StoredSession& previous, const QString& target,
                                           const QString& messageId, const QJsonObject& result,
                                           QString* error) {
    const qint64 revision = previous.contextRevision + 1;
    return appendEventWithMutation(
        previous.id, previous.generation, QStringLiteral("conversation.context_changed"),
        {{"from_context_revision", previous.contextRevision}, {"context_revision", revision}},
        error, [&] {
            QSqlQuery current(database_);
            current.prepare(
                QStringLiteral("SELECT context_revision, worktree_path, last_cursor FROM sessions "
                               "WHERE id = ? AND generation = ?"));
            current.addBindValue(previous.id);
            current.addBindValue(previous.generation);
            if (!current.exec() || !current.next() ||
                current.value(0).toLongLong() != previous.contextRevision ||
                current.value(1).toString() != previous.worktreePath) {
                setError(QStringLiteral("Stale session context."));
                return false;
            }
            if (!recordConversationPath(previous))
                return false;
            StoredSession next = previous;
            next.worktreePath = target;
            next.contextRevision = revision;
            next.lastCursor = current.value(2).toLongLong();
            return upsertSession(next) && recordSessionContext(next) &&
                   storeCommandResult(messageId, result);
        });
}

bool LocalJournal::forgetWorktree(const QString& path) {
    QSqlQuery query(database_);
    query.prepare("DELETE FROM worktree_catalog WHERE path = ?");
    query.addBindValue(path);
    return query.exec();
}

} // namespace ketplus::daemon
