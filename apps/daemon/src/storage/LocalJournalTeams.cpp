// ADR 0114, open point 4: one JSON document per team, a session → team index that makes
// "one team per conversation" a constraint, and the append-only log of updates.
#include "storage/LocalJournal.h"

#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

namespace ketplus::daemon {
namespace {

QString compact(const QJsonObject& value) {
    return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}

QJsonObject parsed(const QString& text) { return QJsonDocument::fromJson(text.toUtf8()).object(); }

} // namespace

bool LocalJournal::initializeTeams() {
    QSqlQuery query(database_);
    const QStringList statements{
        QStringLiteral("CREATE TABLE IF NOT EXISTS teams (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                       "repository TEXT NOT NULL, document TEXT NOT NULL, revision INTEGER NOT "
                       "NULL, created_at TEXT NOT NULL, changed_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS team_members (session_id TEXT PRIMARY KEY, "
                       "team_id INTEGER NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS team_members_team ON team_members(team_id)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS team_updates (id INTEGER PRIMARY KEY "
                       "AUTOINCREMENT, team_id INTEGER NOT NULL, at TEXT NOT NULL, actor TEXT NOT "
                       "NULL, kind TEXT NOT NULL, task_id TEXT NOT NULL DEFAULT '', text TEXT NOT "
                       "NULL, data TEXT NOT NULL DEFAULT '{}', actionable INTEGER NOT NULL DEFAULT "
                       "0, digested INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS team_updates_team ON team_updates(team_id, id)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS team_history_texts (team_id INTEGER NOT "
                       "NULL, task_id TEXT NOT NULL, entry INTEGER NOT NULL, text TEXT NOT NULL, "
                       "PRIMARY KEY (team_id, task_id, entry))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS team_settings (singleton INTEGER PRIMARY KEY "
                       "CHECK(singleton=1), document TEXT NOT NULL)")};
    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            setError(query.lastError().text());
            return false;
        }
    }
    return true;
}

QJsonObject LocalJournal::teamSettings(bool* read) const {
    QSqlQuery query(database_);
    const bool ok =
        query.exec(QStringLiteral("SELECT document FROM team_settings WHERE singleton = 1"));
    if (read)
        *read = ok;
    return ok && query.next() ? parsed(query.value(0).toString()) : QJsonObject{};
}

bool LocalJournal::saveTeamSettings(const QJsonObject& settings, QString* error) {
    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral("INSERT INTO team_settings (singleton, document) VALUES (1, ?) "
                       "ON CONFLICT(singleton) DO UPDATE SET document = excluded.document"));
    query.addBindValue(compact(settings));
    if (query.exec())
        return true;
    if (error)
        *error = query.lastError().text();
    return false;
}

bool LocalJournal::writeTeamMembers(const qint64 teamId, const QStringList& sessions,
                                    QString* error) {
    QSqlQuery clear(database_);
    clear.prepare(QStringLiteral("DELETE FROM team_members WHERE team_id = ?"));
    clear.addBindValue(teamId);
    if (!clear.exec()) {
        *error = clear.lastError().text();
        return false;
    }
    for (const QString& session : sessions) {
        QSqlQuery insert(database_);
        insert.prepare(
            QStringLiteral("INSERT INTO team_members (session_id, team_id) VALUES (?, ?)"));
        insert.addBindValue(session);
        insert.addBindValue(teamId);
        if (!insert.exec()) {
            *error = insert.lastError().text();
            return false;
        }
    }
    return true;
}

std::optional<qint64> LocalJournal::insertTeam(const QString& repository, QJsonObject document,
                                               const QStringList& sessions, QString* error) {
    QString failure;
    if (!database_.transaction()) {
        if (error)
            *error = database_.lastError().text();
        return std::nullopt;
    }
    const QString now = document.value(QStringLiteral("created_at")).toString();
    QSqlQuery insert(database_);
    insert.prepare(QStringLiteral("INSERT INTO teams (repository, document, revision, created_at, "
                                  "changed_at) VALUES (?, '{}', 1, ?, ?)"));
    insert.addBindValue(repository);
    insert.addBindValue(now);
    insert.addBindValue(now);
    if (!insert.exec()) {
        failure = insert.lastError().text();
        database_.rollback();
        if (error)
            *error = failure;
        return std::nullopt;
    }
    const qint64 id = insert.lastInsertId().toLongLong();
    document.insert(QStringLiteral("id"), QString::number(id));
    document.insert(QStringLiteral("revision"), 1);
    QSqlQuery update(database_);
    update.prepare(QStringLiteral("UPDATE teams SET document = ? WHERE id = ?"));
    update.addBindValue(compact(document));
    update.addBindValue(id);
    if (!update.exec() || !writeTeamMembers(id, sessions, &failure)) {
        if (failure.isEmpty())
            failure = update.lastError().text();
        database_.rollback();
        if (error)
            *error = failure;
        return std::nullopt;
    }
    if (!database_.commit()) {
        if (error)
            *error = database_.lastError().text();
        database_.rollback();
        return std::nullopt;
    }
    return id;
}

bool LocalJournal::saveTeam(const qint64 teamId, const qint64 expectedRevision,
                            const QJsonObject& document, const QStringList& sessions,
                            QString* error) {
    QString failure;
    if (!database_.transaction()) {
        if (error)
            *error = database_.lastError().text();
        return false;
    }
    QSqlQuery update(database_);
    update.prepare(QStringLiteral("UPDATE teams SET document = ?, revision = ?, changed_at = ? "
                                  "WHERE id = ? AND revision = ?"));
    update.addBindValue(compact(document));
    update.addBindValue(document.value(QStringLiteral("revision")).toInteger());
    update.addBindValue(document.value(QStringLiteral("changed_at")).toString());
    update.addBindValue(teamId);
    update.addBindValue(expectedRevision);
    if (!update.exec() || update.numRowsAffected() != 1) {
        failure = update.lastError().text();
        if (failure.isEmpty())
            failure = QStringLiteral("The team changed meanwhile.");
    } else if (!writeTeamMembers(teamId, sessions, &failure)) {
        // failure is set.
    } else if (database_.commit()) {
        return true;
    } else {
        failure = database_.lastError().text();
    }
    database_.rollback();
    if (error)
        *error = failure;
    return false;
}

bool LocalJournal::deleteTeam(const qint64 teamId, QString* error) {
    if (!database_.transaction()) {
        if (error)
            *error = database_.lastError().text();
        return false;
    }
    for (const QString& statement :
         {QStringLiteral("DELETE FROM team_updates WHERE team_id = ?"),
          QStringLiteral("DELETE FROM team_history_texts WHERE team_id = ?"),
          QStringLiteral("DELETE FROM team_members WHERE team_id = ?"),
          QStringLiteral("DELETE FROM teams WHERE id = ?")}) {
        QSqlQuery query(database_);
        query.prepare(statement);
        query.addBindValue(teamId);
        if (!query.exec()) {
            if (error)
                *error = query.lastError().text();
            database_.rollback();
            return false;
        }
    }
    if (database_.commit())
        return true;
    if (error)
        *error = database_.lastError().text();
    database_.rollback();
    return false;
}

bool LocalJournal::saveTeamHistoryText(const qint64 teamId, const QString& taskId,
                                       const qsizetype entry, const QString& text, QString* error) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO team_history_texts (team_id, task_id, "
                                 "entry, text) VALUES (?, ?, ?, ?)"));
    query.addBindValue(teamId);
    query.addBindValue(taskId);
    query.addBindValue(static_cast<qint64>(entry));
    query.addBindValue(text);
    if (query.exec())
        return true;
    if (error)
        *error = query.lastError().text();
    return false;
}

std::optional<QString> LocalJournal::teamHistoryText(const qint64 teamId, const QString& taskId,
                                                     const qsizetype entry) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT text FROM team_history_texts WHERE team_id = ? AND "
                                 "task_id = ? AND entry = ?"));
    query.addBindValue(teamId);
    query.addBindValue(taskId);
    query.addBindValue(static_cast<qint64>(entry));
    if (!query.exec() || !query.next())
        return std::nullopt;
    return query.value(0).toString();
}

QList<QJsonObject> LocalJournal::teamDocuments(bool* read) const {
    QList<QJsonObject> documents;
    QSqlQuery query(database_);
    const bool ran = query.exec(QStringLiteral("SELECT document FROM teams ORDER BY id"));
    if (read)
        *read = ran;
    if (!ran)
        return documents;
    while (query.next())
        documents.append(parsed(query.value(0).toString()));
    // A read that stopped part way is not the whole list: report it and return nothing,
    // so the caller keeps what it had instead of a list with teams missing.
    if (query.lastError().isValid()) {
        if (read)
            *read = false;
        return {};
    }
    return documents;
}

std::optional<qint64> LocalJournal::teamOfSession(const QString& sessionId) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT team_id FROM team_members WHERE session_id = ?"));
    query.addBindValue(sessionId);
    if (!query.exec() || !query.next())
        return std::nullopt;
    return query.value(0).toLongLong();
}

std::optional<qint64> LocalJournal::appendTeamUpdate(const TeamUpdateRecord& update,
                                                     QString* error) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("INSERT INTO team_updates (team_id, at, actor, kind, task_id, "
                                 "text, data, actionable) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(update.teamId);
    query.addBindValue(update.at);
    query.addBindValue(update.actor.isNull() ? QStringLiteral("") : update.actor);
    query.addBindValue(update.kind.isNull() ? QStringLiteral("") : update.kind);
    query.addBindValue(update.taskId.isNull() ? QStringLiteral("") : update.taskId);
    query.addBindValue(update.text.isNull() ? QStringLiteral("") : update.text);
    query.addBindValue(compact(update.data));
    query.addBindValue(update.actionable ? 1 : 0);
    if (!query.exec()) {
        if (error)
            *error = query.lastError().text();
        return std::nullopt;
    }
    return query.lastInsertId().toLongLong();
}

QList<TeamUpdateRecord> LocalJournal::teamUpdates(const qint64 teamId, const bool undigestedOnly,
                                                  const int limit, const qint64 beforeId) const {
    QList<TeamUpdateRecord> updates;
    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral("SELECT id, at, actor, kind, task_id, text, data, actionable, digested "
                       "FROM team_updates WHERE team_id = ? %1 %3 ORDER BY id %2 LIMIT ?")
            .arg(undigestedOnly ? QStringLiteral("AND digested = 0") : QString{},
                 undigestedOnly ? QStringLiteral("ASC") : QStringLiteral("DESC"),
                 beforeId > 0 ? QStringLiteral("AND id < ?") : QString{}));
    query.addBindValue(teamId);
    if (beforeId > 0)
        query.addBindValue(beforeId);
    query.addBindValue(limit);
    if (!query.exec())
        return updates;
    while (query.next())
        updates.append({.id = query.value(0).toLongLong(),
                        .teamId = teamId,
                        .at = query.value(1).toString(),
                        .actor = query.value(2).toString(),
                        .kind = query.value(3).toString(),
                        .taskId = query.value(4).toString(),
                        .text = query.value(5).toString(),
                        .data = parsed(query.value(6).toString()),
                        .actionable = query.value(7).toInt() != 0,
                        .digested = query.value(8).toInt() != 0});
    return updates;
}

bool LocalJournal::markTeamUpdatesDigested(const qint64 teamId, const qint64 throughId) {
    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral("UPDATE team_updates SET digested = 1 WHERE team_id = ? AND id <= ?"));
    query.addBindValue(teamId);
    query.addBindValue(throughId);
    return query.exec();
}

} // namespace ketplus::daemon
