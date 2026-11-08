#include "storage/LocalJournal.h"
#include "storage/TranscriptItemSnapshot.h"

#include "protocol/DaemonProtocol.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <limits>

namespace ketplus::daemon {
namespace {

QJsonObject parseObject(const QString& value) {
    const QJsonDocument document = QJsonDocument::fromJson(value.toUtf8());
    return document.isObject() ? document.object() : QJsonObject{};
}

QString compact(const QJsonObject& value) {
    return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}

QString eventItemId(const QJsonObject& payload) {
    const QString direct = payload.value(QStringLiteral("item_id")).toString();
    const QString nested =
        payload.value(QStringLiteral("item")).toObject().value(QStringLiteral("id")).toString();
    const QString result = direct.isEmpty() ? nested : direct;
    return result.isNull() ? QStringLiteral("") : result;
}

QJsonObject replayEvent(const QString& sessionId, const qint64 generation, const QSqlQuery& query) {
    return {{QStringLiteral("session_id"), sessionId},
            {QStringLiteral("session_generation"), generation},
            {QStringLiteral("cursor"), query.value(0).toLongLong()},
            {QStringLiteral("turn_sequence"), query.value(1).toLongLong()},
            {QStringLiteral("event_type"), query.value(2).toString()},
            {QStringLiteral("payload"), parseObject(query.value(3).toString())},
            {QStringLiteral("sent_at"), query.value(4).toString()}};
}

qsizetype replayEventBytes(const QString& sessionId, const QString& type,
                           const QString& encodedPayload, const QString& sentAt) {
    constexpr qsizetype envelopeAllowance = 192;
    return sessionId.toUtf8().size() + type.toUtf8().size() + encodedPayload.toUtf8().size() +
           sentAt.toUtf8().size() + envelopeAllowance;
}

} // namespace

LocalJournal::LocalJournal(const QString& path)
    : connectionName_(QStringLiteral("ketplus-daemon-%1")
                          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))),
      database_(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_)) {
    database_.setDatabaseName(path);
    if (!database_.open()) {
        setError(database_.lastError().text());
        return;
    }
    // Operations reset their own errors. Keep failed startup unusable even if a later
    // operation clears that diagnostic; recovery requires a freshly initialized journal.
    if (!initialize() || !initializeActivityGroups() || !initializeAgents())
        database_.close();
}

LocalJournal::~LocalJournal() {
    database_.close();
    database_ = {};
    QSqlDatabase::removeDatabase(connectionName_);
}

bool LocalJournal::isOpen() const { return database_.isOpen() && error_.isEmpty(); }

QString LocalJournal::errorString() const { return error_; }

bool LocalJournal::initialize() {
    QSqlQuery query(database_);
    const QStringList statements{
        // Takes effect only in a journal without tables: a new one. An older journal moves to
        // it through journal_space::migrateToIncrementalVacuum.
        QStringLiteral("PRAGMA auto_vacuum=INCREMENTAL"),
        QStringLiteral("PRAGMA journal_mode=WAL"),
        QStringLiteral("PRAGMA synchronous=FULL"),
        QStringLiteral("PRAGMA foreign_keys=ON"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS journal_retention_settings ("
            "singleton INTEGER PRIMARY KEY CHECK(singleton=1), "
            "revision INTEGER NOT NULL CHECK(typeof(revision)='integer' AND "
            "revision>0), policy TEXT NOT NULL CHECK(length(CAST(policy AS BLOB))<=4096))"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS journal_size_settings ("
            "singleton INTEGER PRIMARY KEY CHECK(singleton=1), "
            "revision INTEGER NOT NULL CHECK(typeof(revision)='integer' AND revision>0), "
            "policy TEXT NOT NULL CHECK(length(CAST(policy AS BLOB))<=4096))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS journal_attachment_gc ("
                       "singleton INTEGER PRIMARY KEY CHECK(singleton=1), "
                       "manifest_row INTEGER NOT NULL DEFAULT 0 CHECK(manifest_row>=0), "
                       "chunk_row INTEGER NOT NULL DEFAULT 0 CHECK(chunk_row>=0))"),
        QStringLiteral("INSERT OR IGNORE INTO journal_attachment_gc(singleton) VALUES(1)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS journal_gc_state ("
                       "singleton INTEGER PRIMARY KEY CHECK(singleton=1), "
                       "payload_row INTEGER NOT NULL DEFAULT 0 CHECK(payload_row>=0), "
                       "chunk_row INTEGER NOT NULL DEFAULT 0 CHECK(chunk_row>=0))"),
        QStringLiteral("INSERT OR IGNORE INTO journal_gc_state(singleton) VALUES(1)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS journal_ack_gc ("
                       "queue_id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL, "
                       "generation INTEGER NOT NULL, "
                       "UNIQUE(session_id,generation))"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS journal_metadata_gc ("
            "table_name TEXT PRIMARY KEY, age_row INTEGER NOT NULL DEFAULT 0 CHECK(age_row>=0), "
            "cap_row INTEGER NOT NULL DEFAULT 0 CHECK(cap_row>=0))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sessions ("
                       "id TEXT PRIMARY KEY, worktree_path TEXT NOT NULL, adapter TEXT NOT NULL, "
                       "thread_id TEXT NOT NULL, turn_id TEXT NOT NULL, state TEXT NOT NULL, "
                       "generation INTEGER NOT NULL, last_cursor INTEGER NOT NULL, "
                       "current_turn_sequence INTEGER NOT NULL DEFAULT 0, "
                       "title TEXT NOT NULL DEFAULT '', "
                       "configuration_revision INTEGER NOT NULL DEFAULT 1, "
                       "model TEXT NOT NULL DEFAULT '', effort TEXT NOT NULL DEFAULT '', "
                       "execution_policy TEXT NOT NULL DEFAULT 'ask', "
                       "mode TEXT NOT NULL DEFAULT 'default', "
                       "provider_threads TEXT NOT NULL DEFAULT '{}', "
                       "last_turn_started_at TEXT NOT NULL DEFAULT '')"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS worktree_catalog ("
                       "path TEXT PRIMARY KEY, catalog_revision INTEGER NOT NULL DEFAULT 0, "
                       "lifecycle_revision INTEGER NOT NULL DEFAULT 0, updated_at TEXT NOT NULL, "
                       "repository_path TEXT NOT NULL DEFAULT '')"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS events ("
            "session_id TEXT NOT NULL, generation INTEGER NOT NULL, cursor INTEGER NOT NULL, "
            "event_type TEXT NOT NULL, payload TEXT NOT NULL, sent_at TEXT NOT NULL, "
            "turn_sequence INTEGER NOT NULL DEFAULT 0, "
            "PRIMARY KEY(session_id, generation, cursor))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS background_tasks ("
                       "session_id TEXT NOT NULL, generation INTEGER NOT NULL, "
                       "tasks TEXT NOT NULL, PRIMARY KEY(session_id, generation))"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS command_results ("
            "message_id TEXT PRIMARY KEY, result TEXT NOT NULL, completed_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS session_tombstones ("
                       "session_id TEXT NOT NULL, generation INTEGER NOT NULL, "
                       "lifecycle_revision INTEGER NOT NULL, operation_id TEXT NOT NULL, "
                       "deleted_at TEXT NOT NULL, PRIMARY KEY(session_id, generation))"),
        // A mutation token fences keyset walks, including retention and older writers.
        QStringLiteral("CREATE TABLE IF NOT EXISTS session_tombstone_catalog ("
                       "id INTEGER PRIMARY KEY CHECK(id=1), revision TEXT NOT NULL)"),
        QStringLiteral("INSERT OR IGNORE INTO session_tombstone_catalog VALUES "
                       "(1,lower(hex(randomblob(16))))"),
        QStringLiteral("CREATE TRIGGER IF NOT EXISTS session_tombstone_insert_revision "
                       "AFTER INSERT ON session_tombstones BEGIN UPDATE session_tombstone_catalog "
                       "SET revision=lower(hex(randomblob(16))) WHERE id=1; END"),
        QStringLiteral("CREATE TRIGGER IF NOT EXISTS session_tombstone_update_revision "
                       "AFTER UPDATE ON session_tombstones BEGIN UPDATE session_tombstone_catalog "
                       "SET revision=lower(hex(randomblob(16))) WHERE id=1; END"),
        QStringLiteral("CREATE TRIGGER IF NOT EXISTS session_tombstone_delete_revision "
                       "AFTER DELETE ON session_tombstones BEGIN UPDATE session_tombstone_catalog "
                       "SET revision=lower(hex(randomblob(16))) WHERE id=1; END"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS session_deletion_work ("
            "session_id TEXT NOT NULL, generation INTEGER NOT NULL, "
            "deletion TEXT NOT NULL, provider_targets TEXT NOT NULL DEFAULT '[]', "
            "cloud_acked INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(session_id, generation))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS session_deletion_pending ON "
                       "session_deletion_work((provider_targets != '[]' OR cloud_acked=0))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS session_deletion_cloud_pending ON "
                       "session_deletion_work(cloud_acked)"),
        // Deleted generations whose bulky history still awaits bounded removal.
        QStringLiteral("CREATE TABLE IF NOT EXISTS session_history_purge ("
                       "session_id TEXT NOT NULL, generation INTEGER NOT NULL, "
                       "PRIMARY KEY(session_id, generation))"),
        // Forks still copying history (ADR 0094). Only the daemon owns the journal, so a row
        // left at open is an interrupted copy: its rows become purge work.
        QStringLiteral("CREATE TABLE IF NOT EXISTS session_fork_staging ("
                       "session_id TEXT NOT NULL, generation INTEGER NOT NULL, "
                       "PRIMARY KEY(session_id, generation))"),
        QStringLiteral("INSERT OR IGNORE INTO session_history_purge(session_id, generation) "
                       "SELECT session_id, generation FROM session_fork_staging"),
        QStringLiteral("DELETE FROM session_fork_staging"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS cloud_upload_progress ("
            "scope TEXT NOT NULL, session_id TEXT NOT NULL, generation INTEGER NOT NULL, "
            "cursor INTEGER NOT NULL, PRIMARY KEY(scope,session_id,generation))"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS cloud_events ("
            "session_id TEXT NOT NULL, generation INTEGER NOT NULL, cursor INTEGER NOT NULL, "
            "event TEXT NOT NULL, created_at TEXT NOT NULL, "
            "projection_policy_version INTEGER NOT NULL DEFAULT 0, "
            "upload_acked INTEGER NOT NULL DEFAULT 0, "
            "PRIMARY KEY(session_id, generation, cursor))"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS client_cursors ("
            "client_id TEXT NOT NULL, session_id TEXT NOT NULL, generation INTEGER NOT NULL, "
            "cursor INTEGER NOT NULL, acknowledged_at TEXT NOT NULL, "
            "PRIMARY KEY(client_id, session_id, generation))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS client_cursors_generation ON "
                       "client_cursors(session_id,generation,cursor)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS replay_events ("
            "session_id TEXT NOT NULL, generation INTEGER NOT NULL, cursor INTEGER NOT NULL, "
            "event_type TEXT NOT NULL, item_id TEXT NOT NULL, payload TEXT NOT NULL, "
            "sent_at TEXT NOT NULL, turn_sequence INTEGER NOT NULL DEFAULT 0, "
            "PRIMARY KEY(session_id, generation, cursor))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS replay_events_item ON "
                       "replay_events(session_id, generation, item_id, event_type)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS replay_projection_state ("
            "session_id TEXT NOT NULL, generation INTEGER NOT NULL, first_cursor INTEGER NOT NULL, "
            "last_cursor INTEGER NOT NULL, PRIMARY KEY(session_id, generation))"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS large_event_payloads ("
            "session_id TEXT NOT NULL, generation INTEGER NOT NULL, transfer_id TEXT NOT NULL, "
            "event_type TEXT NOT NULL, item_id TEXT NOT NULL, revision INTEGER NOT NULL, "
            "digest_sha256 TEXT NOT NULL, "
            "payload BLOB NOT NULL, created_at TEXT NOT NULL, "
            "PRIMARY KEY(session_id, generation, transfer_id))"),
        // Fragment-sized slices of each large payload, so serving one fragment reads one row
        // instead of materializing the whole (up to 32 MiB) blob per request.
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS large_event_payload_chunks ("
            "session_id TEXT NOT NULL, generation INTEGER NOT NULL, transfer_id TEXT NOT NULL, "
            "chunk_offset INTEGER NOT NULL, payload BLOB NOT NULL, "
            "PRIMARY KEY(session_id, generation, transfer_id, chunk_offset))"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS staged_attachments ("
            "session_id TEXT NOT NULL, generation INTEGER NOT NULL, attachment_id TEXT NOT NULL, "
            "revision INTEGER NOT NULL, name TEXT NOT NULL, media_type TEXT NOT NULL, "
            "total_bytes INTEGER NOT NULL, chunk_count INTEGER NOT NULL, digest_sha256 TEXT NOT "
            "NULL, "
            "status TEXT NOT NULL, created_at TEXT NOT NULL, "
            "PRIMARY KEY(session_id, generation, attachment_id))"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS staged_attachment_chunks ("
            "session_id TEXT NOT NULL, generation INTEGER NOT NULL, attachment_id TEXT NOT NULL, "
            "revision INTEGER NOT NULL, chunk_index INTEGER NOT NULL, digest_sha256 TEXT NOT NULL, "
            "payload BLOB NOT NULL, PRIMARY KEY(session_id, generation, attachment_id, "
            "chunk_index))"),
    };
    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            setError(query.lastError().text());
            return false;
        }
    }
    return migrateTranscriptSchema() && purgeExpiredAttachments() && backfillTurnSequences() &&
           backfillReplayProjection() && initializeRemoteAudit() && pruneAcknowledgedEvents() &&
           pruneRetention();
}

bool LocalJournal::migrateTranscriptSchema() {
    const auto ensureColumn = [this](const QString& table, const QString& column,
                                     const QString& declaration) {
        QSqlQuery columns(database_);
        if (!columns.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
            setError(columns.lastError().text());
            return false;
        }
        while (columns.next()) {
            if (columns.value(1).toString() == column) {
                return true;
            }
        }
        QSqlQuery alter(database_);
        if (!alter.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3")
                            .arg(table, column, declaration))) {
            setError(alter.lastError().text());
            return false;
        }
        return true;
    };
    if (!ensureColumn(QStringLiteral("sessions"), QStringLiteral("context_revision"),
                      QStringLiteral("INTEGER NOT NULL DEFAULT 1")) ||
        !ensureColumn(QStringLiteral("sessions"), QStringLiteral("current_turn_sequence"),
                      QStringLiteral("INTEGER NOT NULL DEFAULT 0")) ||
        !ensureColumn(QStringLiteral("events"), QStringLiteral("turn_sequence"),
                      QStringLiteral("INTEGER NOT NULL DEFAULT 0")) ||
        !ensureColumn(QStringLiteral("replay_events"), QStringLiteral("turn_sequence"),
                      QStringLiteral("INTEGER NOT NULL DEFAULT 0")) ||
        !ensureColumn(QStringLiteral("sessions"), QStringLiteral("title"),
                      QStringLiteral("TEXT NOT NULL DEFAULT ''")) ||
        !ensureColumn(QStringLiteral("sessions"), QStringLiteral("configuration_revision"),
                      QStringLiteral("INTEGER NOT NULL DEFAULT 1")) ||
        !ensureColumn(QStringLiteral("sessions"), QStringLiteral("model"),
                      QStringLiteral("TEXT NOT NULL DEFAULT ''")) ||
        !ensureColumn(QStringLiteral("sessions"), QStringLiteral("effort"),
                      QStringLiteral("TEXT NOT NULL DEFAULT ''")) ||
        !ensureColumn(QStringLiteral("sessions"), QStringLiteral("execution_policy"),
                      QStringLiteral("TEXT NOT NULL DEFAULT 'ask'")) ||
        !ensureColumn(QStringLiteral("sessions"), QStringLiteral("mode"),
                      QStringLiteral("TEXT NOT NULL DEFAULT 'default'")) ||
        !ensureColumn(QStringLiteral("sessions"), QStringLiteral("compact_window"),
                      QStringLiteral("INTEGER NOT NULL DEFAULT 0")) ||
        !ensureColumn(QStringLiteral("sessions"), QStringLiteral("provider_threads"),
                      QStringLiteral("TEXT NOT NULL DEFAULT '{}'")) ||
        !ensureColumn(QStringLiteral("sessions"), QStringLiteral("last_seen_cursor"),
                      QStringLiteral("INTEGER NOT NULL DEFAULT 0")) ||
        !ensureColumn(QStringLiteral("sessions"), QStringLiteral("last_turn_started_at"),
                      QStringLiteral("TEXT NOT NULL DEFAULT ''")) ||
        !ensureColumn(QStringLiteral("replay_projection_state"), QStringLiteral("revision"),
                      QStringLiteral("INTEGER NOT NULL DEFAULT 0")) ||
        // The repository a linked worktree belonged to, kept for after the worktree is deleted.
        !ensureColumn(QStringLiteral("worktree_catalog"), QStringLiteral("repository_path"),
                      QStringLiteral("TEXT NOT NULL DEFAULT ''")) ||
        !ensureColumn(QStringLiteral("cloud_events"), QStringLiteral("projection_policy_version"),
                      QStringLiteral("INTEGER NOT NULL DEFAULT 0")) ||
        !ensureColumn(QStringLiteral("cloud_events"), QStringLiteral("upload_acked"),
                      QStringLiteral("INTEGER NOT NULL DEFAULT 0")) ||
        // -1 marks legacy rows whose bytes live only in payload (no chunk rows).
        !ensureColumn(QStringLiteral("large_event_payloads"), QStringLiteral("total_bytes"),
                      QStringLiteral("INTEGER NOT NULL DEFAULT -1"))) {
        return false;
    }
    // Conversations saved before the catalog existed name worktrees it never recorded.
    QSqlQuery catalog(database_);
    if (!catalog.exec(QStringLiteral("INSERT OR IGNORE INTO worktree_catalog(path, updated_at) "
                                     "SELECT DISTINCT worktree_path, "
                                     "strftime('%Y-%m-%dT%H:%M:%fZ', 'now') FROM sessions"))) {
        setError(catalog.lastError().text());
        return false;
    }
    // Conversations from before the column existed take their latest user message's time.
    // Pruned events may have left it only in the replay projection.
    QSqlQuery turnStarts(database_);
    if (!turnStarts.exec(QStringLiteral(
            "UPDATE sessions SET last_turn_started_at = max("
            "COALESCE((SELECT MAX(sent_at) FROM events e WHERE e.session_id = sessions.id "
            "AND e.generation = sessions.generation AND e.event_type = 'user.message'), ''), "
            "COALESCE((SELECT MAX(sent_at) FROM replay_events r WHERE r.session_id = sessions.id "
            "AND r.generation = sessions.generation AND r.event_type = 'user.message'), '')) "
            "WHERE last_turn_started_at = ''"))) {
        setError(turnStarts.lastError().text());
        return false;
    }
    if (!initializeContexts())
        return false;
    QSqlQuery index(database_);
    // Retention probes a specific turn, not the whole session's raw backlog.
    if (!index.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS events_turn ON "
                                   "events(session_id, generation, turn_sequence)"))) {
        setError(index.lastError().text());
        return false;
    }
    if (!index.exec(
            QStringLiteral("CREATE INDEX IF NOT EXISTS replay_events_turn ON "
                           "replay_events(session_id, generation, turn_sequence, cursor)"))) {
        setError(index.lastError().text());
        return false;
    }
    if (!index.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS replay_events_item_cursor ON "
            "replay_events(session_id, generation, item_id, turn_sequence, cursor)"))) {
        setError(index.lastError().text());
        return false;
    }
    // Blob GC must seek the transfer instead of parsing every replay row for an item.
    // This initializer also runs when opening journals created before this index.
    if (!index.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS replay_events_item_transfer ON "
                                   "replay_events(session_id, generation, item_id, "
                                   "json_extract(payload,'$.large_event.transfer_id'))"))) {
        setError(index.lastError().text());
        return false;
    }
    return true;
}

bool LocalJournal::backfillTurnSequences() {
    QSqlQuery sessions(database_);
    if (!sessions.exec(
            QStringLiteral("SELECT s.id, s.generation FROM sessions s WHERE "
                           "(s.current_turn_sequence = 0 OR s.title = '') "
                           "AND EXISTS (SELECT 1 FROM events e WHERE e.session_id = s.id "
                           "AND e.generation = s.generation AND e.event_type = 'user.message') "
                           // Recounting from surviving rows is only correct when none were
                           // pruned; otherwise turn 8 would be renumbered as turn 1.
                           "AND (SELECT MIN(e.cursor) FROM events e WHERE e.session_id = s.id "
                           "AND e.generation = s.generation) = 1"))) {
        setError(sessions.lastError().text());
        return false;
    }
    QList<QPair<QString, qint64>> targets;
    while (sessions.next()) {
        targets.append({sessions.value(0).toString(), sessions.value(1).toLongLong()});
    }
    if (!database_.transaction()) {
        setError(database_.lastError().text());
        return false;
    }
    for (const auto& target : targets) {
        QSqlQuery events(database_);
        events.prepare(
            QStringLiteral("SELECT cursor, event_type, payload FROM events WHERE session_id = ? "
                           "AND generation = ? ORDER BY cursor"));
        events.addBindValue(target.first);
        events.addBindValue(target.second);
        if (!events.exec()) {
            database_.rollback();
            setError(events.lastError().text());
            return false;
        }
        qint64 turnSequence = 0;
        QString title;
        while (events.next()) {
            if (events.value(1).toString() == QStringLiteral("user.message")) {
                ++turnSequence;
                if (title.isEmpty()) {
                    title = parseObject(events.value(2).toString())
                                .value(QStringLiteral("text"))
                                .toString()
                                .simplified()
                                .left(120);
                }
            }
            for (const QString& table :
                 {QStringLiteral("events"), QStringLiteral("replay_events")}) {
                QSqlQuery update(database_);
                update.prepare(QStringLiteral("UPDATE %1 SET turn_sequence = ? WHERE "
                                              "session_id = ? AND generation = ? AND cursor = ? "
                                              "AND turn_sequence = 0")
                                   .arg(table));
                update.addBindValue(turnSequence);
                update.addBindValue(target.first);
                update.addBindValue(target.second);
                update.addBindValue(events.value(0));
                if (!update.exec()) {
                    database_.rollback();
                    setError(update.lastError().text());
                    return false;
                }
            }
        }
        QSqlQuery updateSession(database_);
        updateSession.prepare(QStringLiteral(
            "UPDATE sessions SET current_turn_sequence = MAX(current_turn_sequence, ?), "
            "title = CASE WHEN title = '' THEN ? ELSE title END WHERE id = ? AND generation = ?"));
        updateSession.addBindValue(turnSequence);
        updateSession.addBindValue(title);
        updateSession.addBindValue(target.first);
        updateSession.addBindValue(target.second);
        if (!updateSession.exec()) {
            database_.rollback();
            setError(updateSession.lastError().text());
            return false;
        }
    }
    if (!database_.commit()) {
        database_.rollback();
        setError(database_.lastError().text());
        return false;
    }
    return true;
}

bool LocalJournal::backfillReplayProjection() {
    struct ProjectionTarget final {
        QString sessionId;
        qint64 generation{1};
        qint64 lastCursor{0};
        qint64 projectedCursor{0};
    };
    QList<ProjectionTarget> targets;
    QSqlQuery sessions(database_);
    if (!sessions.exec(QStringLiteral(
            "SELECT s.id, s.generation, s.last_cursor, COALESCE(p.last_cursor, 0) "
            "FROM sessions s LEFT JOIN replay_projection_state p ON p.session_id = s.id "
            "AND p.generation = s.generation WHERE COALESCE(p.last_cursor, 0) < s.last_cursor"))) {
        setError(sessions.lastError().text());
        return false;
    }
    while (sessions.next()) {
        targets.append({sessions.value(0).toString(), sessions.value(1).toLongLong(),
                        sessions.value(2).toLongLong(), sessions.value(3).toLongLong()});
    }
    if (targets.isEmpty()) {
        return true;
    }
    if (!database_.transaction()) {
        setError(database_.lastError().text());
        return false;
    }
    for (const ProjectionTarget& target : targets) {
        QSqlQuery events(database_);
        events.prepare(QStringLiteral("SELECT cursor, turn_sequence, event_type, payload, sent_at "
                                      "FROM events WHERE session_id = ? "
                                      "AND generation = ? AND cursor > ? ORDER BY cursor"));
        events.addBindValue(target.sessionId);
        events.addBindValue(target.generation);
        events.addBindValue(target.projectedCursor);
        if (!events.exec()) {
            database_.rollback();
            setError(events.lastError().text());
            return false;
        }
        qint64 projectedCursor = target.projectedCursor;
        while (events.next()) {
            projectedCursor = events.value(0).toLongLong();
            QString failure;
            if (!updateReplayProjection(target.sessionId, target.generation, projectedCursor,
                                        events.value(1).toLongLong(), events.value(2).toString(),
                                        events.value(3).toString(), events.value(4).toString(),
                                        &failure)) {
                database_.rollback();
                setError(failure);
                return false;
            }
        }
        if (projectedCursor < target.lastCursor) {
            QSqlQuery state(database_);
            state.prepare(QStringLiteral(
                "INSERT INTO replay_projection_state(session_id, generation, first_cursor, "
                "last_cursor) VALUES(?, ?, ?, ?) ON CONFLICT(session_id, generation) DO UPDATE "
                "SET last_cursor = MAX(last_cursor, excluded.last_cursor)"));
            state.addBindValue(target.sessionId);
            state.addBindValue(target.generation);
            state.addBindValue(target.lastCursor + 1);
            state.addBindValue(target.lastCursor);
            if (!state.exec()) {
                database_.rollback();
                setError(state.lastError().text());
                return false;
            }
        }
    }
    if (!database_.commit()) {
        database_.rollback();
        setError(database_.lastError().text());
        return false;
    }
    return true;
}

bool LocalJournal::updateReplayProjection(const QString& sessionId, const qint64 generation,
                                          const qint64 cursor, const qint64 turnSequence,
                                          const QString& type, const QString& encodedPayload,
                                          const QString& sentAt, QString* error) {
    const QJsonObject payload = parseObject(encodedPayload);
    const QString itemId = eventItemId(payload);
    if (type == QStringLiteral("transcript.large_chunk")) {
        QSqlQuery state(database_);
        state.prepare(QStringLiteral(
            "INSERT INTO replay_projection_state(session_id, generation, first_cursor, "
            "last_cursor) VALUES(?, ?, ?, ?) ON CONFLICT(session_id, generation) DO UPDATE SET "
            "first_cursor = MIN(first_cursor, excluded.first_cursor), "
            "last_cursor = MAX(last_cursor, excluded.last_cursor)"));
        state.addBindValue(sessionId);
        state.addBindValue(generation);
        state.addBindValue(cursor);
        state.addBindValue(cursor);
        if (!state.exec() || state.numRowsAffected() != 1) {
            if (error != nullptr) {
                *error = state.lastError().isValid()
                             ? state.lastError().text()
                             : QStringLiteral("Replay cursor state was not stored.");
            }
            return false;
        }
        return true;
    }
    const bool hasLargeAssistantSnapshot =
        isAssistantItem(payload) &&
        !payload.value(QStringLiteral("large_event")).toObject().isEmpty();
    if (type == QStringLiteral("item.completed") && !itemId.isEmpty() &&
        (hasAuthoritativeAssistantText(payload) || hasLargeAssistantSnapshot ||
         (payload.value("item").toObject().value("type") == "commandExecution" &&
          payload.value("item").toObject().value("aggregatedOutput").isString()))) {
        QSqlQuery removeDeltas(database_);
        const QString deltaPredicate = QStringLiteral(
            "session_id = ? AND generation = ? AND item_id = ? AND turn_sequence = ? "
            "AND event_type IN ('assistant.delta', 'command.output_delta')");
        removeDeltas.prepare(QStringLiteral("DELETE FROM replay_events WHERE ") + deltaPredicate);
        removeDeltas.addBindValue(sessionId);
        removeDeltas.addBindValue(generation);
        removeDeltas.addBindValue(itemId);
        removeDeltas.addBindValue(turnSequence);
        if (!removeDeltas.exec()) {
            if (error != nullptr) {
                *error = removeDeltas.lastError().text();
            }
            return false;
        }
        QSqlQuery remaining(database_);
        remaining.prepare(QStringLiteral("SELECT EXISTS(SELECT 1 FROM replay_events WHERE ") +
                          deltaPredicate + ')');
        for (const auto& value : removeDeltas.boundValues())
            remaining.addBindValue(value);
        if (!remaining.exec() || !remaining.next() || remaining.value(0).toBool()) {
            if (error)
                *error =
                    remaining.lastError().isValid()
                        ? remaining.lastError().text()
                        : QStringLiteral("Superseded replay deltas were not completely removed.");
            return false;
        }
        if (removeDeltas.numRowsAffected() > 0) {
            QSqlQuery revision(database_);
            revision.prepare(
                QStringLiteral("UPDATE replay_projection_state SET revision = "
                               "revision + 1 WHERE session_id = ? AND generation = ?"));
            revision.addBindValue(sessionId);
            revision.addBindValue(generation);
            if (!revision.exec() || revision.numRowsAffected() != 1) {
                if (error != nullptr) {
                    *error = revision.lastError().isValid()
                                 ? revision.lastError().text()
                                 : QStringLiteral("Replay revision was not stored.");
                }
                return false;
            }
        }
    }
    {
        QSqlQuery insert(database_);
        insert.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO replay_events(session_id, generation, cursor, event_type, "
            "item_id, payload, sent_at, turn_sequence) VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
        insert.addBindValue(sessionId);
        insert.addBindValue(generation);
        insert.addBindValue(cursor);
        insert.addBindValue(type);
        insert.addBindValue(itemId);
        insert.addBindValue(encodedPayload);
        insert.addBindValue(sentAt);
        insert.addBindValue(turnSequence);
        if (!insert.exec() || insert.numRowsAffected() != 1) {
            if (error != nullptr) {
                *error = insert.lastError().isValid()
                             ? insert.lastError().text()
                             : QStringLiteral("Replay event was not stored.");
            }
            return false;
        }
    }
    QSqlQuery state(database_);
    state.prepare(QStringLiteral(
        "INSERT INTO replay_projection_state(session_id, generation, first_cursor, last_cursor) "
        "VALUES(?, ?, ?, ?) ON CONFLICT(session_id, generation) DO UPDATE SET "
        "first_cursor = MIN(first_cursor, excluded.first_cursor), "
        "last_cursor = MAX(last_cursor, excluded.last_cursor)"));
    state.addBindValue(sessionId);
    state.addBindValue(generation);
    state.addBindValue(cursor);
    state.addBindValue(cursor);
    if (!state.exec() || state.numRowsAffected() != 1) {
        if (error != nullptr) {
            *error = state.lastError().isValid()
                         ? state.lastError().text()
                         : QStringLiteral("Replay cursor state was not stored.");
        }
        return false;
    }
    return true;
}

bool LocalJournal::upsertSession(const StoredSession& session) {
    if (!sessionTombstone(session.id, session.generation).sessionId.isEmpty()) {
        setError(
            QStringLiteral("A tombstone prevents this session generation from being restored."));
        return false;
    }
    QSqlQuery existing(database_);
    existing.prepare(QStringLiteral("SELECT 1 FROM sessions WHERE id = ? AND generation = ?"));
    existing.addBindValue(session.id);
    existing.addBindValue(session.generation);
    if (!existing.exec()) {
        setError(existing.lastError().text());
        return false;
    }
    const bool isNew = !existing.next();
    existing.finish();
    // A new session must not adopt rows that a staged fork or a pending purge still owns.
    if (isNew) {
        existing.prepare(QStringLiteral(
            "SELECT 1 FROM session_fork_staging WHERE session_id = ? AND generation = ? "
            "UNION ALL SELECT 1 FROM session_history_purge WHERE session_id = ? AND "
            "generation = ?"));
        existing.addBindValue(session.id);
        existing.addBindValue(session.generation);
        existing.addBindValue(session.id);
        existing.addBindValue(session.generation);
        if (!existing.exec() || existing.next() || existing.lastError().isValid()) {
            setError(existing.lastError().isValid()
                         ? existing.lastError().text()
                         : QStringLiteral("This session identity is still being cleaned up."));
            return false;
        }
    }
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT INTO sessions(id, worktree_path, adapter, thread_id, turn_id, state, "
        "generation, last_cursor, current_turn_sequence, title, configuration_revision, model, "
        "effort, execution_policy, mode, provider_threads, last_seen_cursor, context_revision, "
        "compact_window) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET worktree_path=excluded.worktree_path, "
        "adapter=excluded.adapter, thread_id=excluded.thread_id, turn_id=excluded.turn_id, "
        "state=excluded.state, "
        // appendEvent owns the cursor counters. A stale in-memory copy must not move them back
        // within a generation, or the next append collides with an existing cursor.
        "last_cursor=CASE WHEN excluded.generation = sessions.generation "
        "THEN MAX(sessions.last_cursor, excluded.last_cursor) ELSE excluded.last_cursor END, "
        "current_turn_sequence=CASE WHEN excluded.generation = sessions.generation "
        "THEN MAX(sessions.current_turn_sequence, excluded.current_turn_sequence) "
        "ELSE excluded.current_turn_sequence END, "
        "generation=excluded.generation, title=excluded.title, "
        "configuration_revision=excluded.configuration_revision, model=excluded.model, "
        "effort=excluded.effort, execution_policy=excluded.execution_policy, mode=excluded.mode, "
        "provider_threads=excluded.provider_threads, context_revision=excluded.context_revision, "
        "compact_window=excluded.compact_window, "
        "last_seen_cursor=CASE WHEN excluded.generation = sessions.generation "
        "THEN MAX(sessions.last_seen_cursor, excluded.last_seen_cursor) "
        "ELSE excluded.last_seen_cursor END "
        "WHERE excluded.generation != sessions.generation OR "
        "excluded.context_revision > sessions.context_revision OR "
        "(excluded.context_revision = sessions.context_revision AND "
        "excluded.worktree_path = sessions.worktree_path)"));
    query.addBindValue(session.id);
    query.addBindValue(session.worktreePath);
    query.addBindValue(session.adapter);
    query.addBindValue(session.threadId.isNull() ? QStringLiteral("") : session.threadId);
    query.addBindValue(session.turnId.isNull() ? QStringLiteral("") : session.turnId);
    query.addBindValue(session.state);
    query.addBindValue(session.generation);
    query.addBindValue(session.lastCursor);
    query.addBindValue(session.currentTurnSequence);
    query.addBindValue(session.title.isNull() ? QStringLiteral("") : session.title);
    query.addBindValue(session.configurationRevision);
    query.addBindValue(session.model.isNull() ? QStringLiteral("") : session.model);
    query.addBindValue(session.effort.isNull() ? QStringLiteral("") : session.effort);
    query.addBindValue(session.executionPolicy.isNull() ? QStringLiteral("ask")
                                                        : session.executionPolicy);
    query.addBindValue(session.mode.isNull() ? QStringLiteral("default") : session.mode);
    query.addBindValue(session.providerThreads.isEmpty() ? QStringLiteral("{}")
                                                         : session.providerThreads);
    query.addBindValue(session.lastSeenCursor);
    query.addBindValue(session.contextRevision);
    query.addBindValue(session.compactWindow);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    if (query.numRowsAffected() != 1) {
        setError(QStringLiteral("Stale session context."));
        return false;
    }
    if (isNew && !recordSessionContext(session))
        return false;
    QSqlQuery catalog(database_);
    catalog.prepare(QStringLiteral(
        "INSERT INTO worktree_catalog(path, catalog_revision, lifecycle_revision, updated_at) "
        "VALUES(?, 0, 0, ?) ON CONFLICT(path) DO UPDATE SET updated_at=excluded.updated_at"));
    catalog.addBindValue(session.worktreePath);
    catalog.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!catalog.exec()) {
        setError(catalog.lastError().text());
        return false;
    }
    return true;
}

bool LocalJournal::markSessionSeen(const QString& sessionId, const qint64 generation,
                                   const qint64 cursor) {
    if (cursor < 0)
        return false;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("UPDATE sessions SET last_seen_cursor = MAX(last_seen_cursor, ?) "
                                 "WHERE id = ? AND generation = ?"));
    query.addBindValue(cursor);
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool LocalJournal::deleteSession(const QString& sessionId, const qint64 generation,
                                 QString* error) {
    return deleteSession(sessionId, generation,
                         QStringLiteral("local:%1:%2").arg(sessionId).arg(generation), 1, error);
}

bool LocalJournal::deleteSession(const QString& sessionId, const qint64 generation,
                                 const QString& operationId, const qint64 lifecycleRevision,
                                 QString* error, const QJsonArray& providerTargets) {
    const SessionTombstone existing = sessionTombstone(sessionId, generation);
    if (!existing.sessionId.isEmpty()) {
        if (existing.operationId == operationId && existing.lifecycleRevision == lifecycleRevision)
            return true;
        if (error != nullptr)
            *error = QStringLiteral("The session generation was deleted by another operation.");
        return false;
    }
    if (operationId.isEmpty() || lifecycleRevision < 1) {
        if (error != nullptr)
            *error = QStringLiteral("The deletion operation or lifecycle revision is invalid.");
        return false;
    }
    if (!database_.transaction()) {
        if (error != nullptr)
            *error = database_.lastError().text();
        return false;
    }
    bool transaction = true;
    const auto rollback = qScopeGuard([&] {
        if (transaction)
            database_.rollback();
    });
    QSqlQuery tombstone(database_);
    tombstone.prepare(
        QStringLiteral("INSERT INTO session_tombstones(session_id, generation, lifecycle_revision, "
                       "operation_id, deleted_at) VALUES(?, ?, ?, ?, ?)"));
    tombstone.addBindValue(sessionId);
    tombstone.addBindValue(generation);
    tombstone.addBindValue(lifecycleRevision);
    tombstone.addBindValue(operationId);
    tombstone.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!tombstone.exec() || tombstone.numRowsAffected() != 1) {
        if (error != nullptr)
            *error = tombstone.lastError().isValid()
                         ? tombstone.lastError().text()
                         : QStringLiteral("The deletion tombstone was not stored.");
        return false;
    }
    const QJsonObject deletion{
        {"sessionId", sessionId},
        {"sessionGeneration", generation},
        {"lifecycleRevision", lifecycleRevision},
        {"operationId", operationId},
        {"deletedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    QSqlQuery cleanup(database_);
    cleanup.prepare(
        "INSERT INTO session_deletion_work(session_id,generation,deletion,provider_targets) "
        "VALUES(?,?,?,?)");
    cleanup.addBindValue(sessionId);
    cleanup.addBindValue(generation);
    cleanup.addBindValue(QString::fromUtf8(QJsonDocument(deletion).toJson(QJsonDocument::Compact)));
    cleanup.addBindValue(
        QString::fromUtf8(QJsonDocument(providerTargets).toJson(QJsonDocument::Compact)));
    if (!cleanup.exec() || cleanup.numRowsAffected() != 1) {
        if (error)
            *error = cleanup.lastError().isValid()
                         ? cleanup.lastError().text()
                         : QStringLiteral("The deletion work was not stored.");
        return false;
    }
    if (QString agentError; !releaseAgentState(sessionId, generation, &agentError)) {
        if (error != nullptr)
            *error = agentError;
        return false;
    }
    // Per-generation state is removed here; bulky history gets one bounded batch and
    // a purge marker for the rest (ADR 0093). The commit makes all of it unreachable.
    for (const QString& table :
         {QStringLiteral("background_tasks"), QStringLiteral("activity_heads"),
          QStringLiteral("replay_projection_state"), QStringLiteral("client_cursors"),
          QStringLiteral("staged_attachments"), QStringLiteral("session_contexts")}) {
        QSqlQuery query(database_);
        query.prepare(
            QStringLiteral("DELETE FROM %1 WHERE session_id = ? AND generation = ?").arg(table));
        query.addBindValue(sessionId);
        query.addBindValue(generation);
        if (!query.exec()) {
            if (error != nullptr)
                *error = query.lastError().text();
            return false;
        }
        query.finish();
        // A successful statement can still ignore rows (for example a trigger).
        // Never commit a tombstone while this generation's history remains.
        query.prepare(QStringLiteral("SELECT 1 FROM %1 WHERE session_id=? AND generation=? "
                                     "LIMIT 1")
                          .arg(table));
        query.addBindValue(sessionId);
        query.addBindValue(generation);
        if (!query.exec() || query.next() || query.lastError().isValid()) {
            if (error != nullptr)
                *error = query.lastError().isValid()
                             ? query.lastError().text()
                             : QStringLiteral("Session history cleanup was incomplete.");
            return false;
        }
    }
    if (!startHistoryPurge(sessionId, generation, error))
        return false;
    QSqlQuery session(database_);
    session.prepare(QStringLiteral("DELETE FROM sessions WHERE id = ? AND generation = ?"));
    session.addBindValue(sessionId);
    session.addBindValue(generation);
    if (!session.exec() || session.numRowsAffected() != 1) {
        if (error != nullptr)
            *error = session.lastError().text().isEmpty()
                         ? QStringLiteral("The session no longer exists.")
                         : session.lastError().text();
        return false;
    }
    if (!database_.commit()) {
        if (error != nullptr)
            *error = database_.lastError().text();
        return false;
    }
    transaction = false;
    // Retention failure must not turn a committed delete into a reported failure.
    if (!pruneRetention())
        qWarning("Journal retention pass failed: %s", qUtf8Printable(error_));
    return true;
}

namespace detail {
// Whether nothing retained names the large payload `p` any longer: no raw event still waiting for
// a client (manifest or chunk), and no compacted replay manifest. Replay rows are found through
// the exact transfer expression index. Unary + removes the TEXT affinity of p.transfer_id
// so SQLite can use the fourth expression key. Raw events await client acknowledgement.
QString largePayloadUnreferenced() {
    return QStringLiteral(
        "NOT EXISTS (SELECT 1 FROM events e WHERE e.session_id = p.session_id AND e.generation = "
        "p.generation AND instr(e.payload, p.transfer_id) > 0 AND (json_extract(e.payload, "
        "'$.transfer_id') = p.transfer_id OR json_extract(e.payload, '$.large_event.transfer_id') "
        "= "
        "p.transfer_id)) AND NOT EXISTS (SELECT 1 FROM replay_events r WHERE r.session_id = "
        "p.session_id AND r.generation = p.generation AND r.item_id = p.item_id AND "
        "json_extract(r.payload, '$.large_event.transfer_id') = +p.transfer_id)");
}
} // namespace detail

bool LocalJournal::pruneUnreferencedLargePayloads(const QString& sessionId, const qint64 generation,
                                                  const QStringList& transferIds) {
    for (const QString& transferId : transferIds) {
        QSqlQuery unreferenced(database_);
        unreferenced.prepare(QStringLiteral("SELECT 1 FROM large_event_payloads p WHERE "
                                            "p.session_id = ? AND p.generation = ? AND "
                                            "p.transfer_id = ? AND %1")
                                 .arg(detail::largePayloadUnreferenced()));
        unreferenced.addBindValue(sessionId);
        unreferenced.addBindValue(generation);
        unreferenced.addBindValue(transferId);
        if (!unreferenced.exec()) {
            setError(unreferenced.lastError().text());
            return false;
        }
        if (!unreferenced.next())
            continue;
        for (const QString& table : {QStringLiteral("large_event_payload_chunks"),
                                     QStringLiteral("large_event_payloads")}) {
            QSqlQuery remove(database_);
            remove.prepare(QStringLiteral("DELETE FROM %1 WHERE session_id = ? AND generation = ? "
                                          "AND transfer_id = ?")
                               .arg(table));
            remove.addBindValue(sessionId);
            remove.addBindValue(generation);
            remove.addBindValue(transferId);
            if (!remove.exec()) {
                setError(remove.lastError().text());
                return false;
            }
        }
    }
    return true;
}

SessionTombstone LocalJournal::sessionTombstone(const QString& sessionId,
                                                const qint64 generation) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT session_id, generation, lifecycle_revision, operation_id, deleted_at "
        "FROM session_tombstones WHERE session_id = ? AND generation = ?"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec()) {
        setError(query.lastError().text());
        return {};
    }
    if (!query.next())
        return {};
    return {query.value(0).toString(), query.value(1).toLongLong(), query.value(2).toLongLong(),
            query.value(3).toString(), query.value(4).toString()};
}

std::optional<QSet<QString>> LocalJournal::sessionIds() const {
    QSqlQuery query(database_);
    query.setForwardOnly(true);
    if (!query.exec(QStringLiteral("SELECT id FROM sessions"))) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    QSet<QString> ids;
    while (query.next())
        ids.insert(query.value(0).toString());
    if (query.lastError().isValid()) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    return ids;
}

SessionHead LocalJournal::sessionHead(const QString& sessionId) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT generation, last_cursor FROM sessions WHERE id = ?"));
    query.addBindValue(sessionId);
    if (!query.exec()) {
        setError(query.lastError().text());
        return {};
    }
    if (!query.next())
        return {};
    return {query.value(0).toLongLong(), query.value(1).toLongLong()};
}

std::optional<QList<StoredSession>> LocalJournal::readSessions() const {
    QList<StoredSession> result;
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral(
            "SELECT id, worktree_path, adapter, thread_id, turn_id, state, generation, "
            "last_cursor, current_turn_sequence, title, configuration_revision, model, effort, "
            "execution_policy, mode, provider_threads, last_seen_cursor, last_turn_started_at, "
            "context_revision, compact_window "
            "FROM sessions ORDER BY id"))) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    while (query.next()) {
        result.append({query.value(0).toString(),    query.value(1).toString(),
                       query.value(2).toString(),    query.value(3).toString(),
                       query.value(4).toString(),    query.value(5).toString(),
                       query.value(6).toLongLong(),  query.value(7).toLongLong(),
                       query.value(8).toLongLong(),  query.value(9).toString(),
                       query.value(10).toLongLong(), query.value(11).toString(),
                       query.value(12).toString(),   query.value(13).toString(),
                       query.value(14).toString(),   query.value(15).toString(),
                       query.value(16).toLongLong(), query.value(17).toString(),
                       query.value(18).toLongLong(), query.value(19).toLongLong()});
    }
    if (query.lastError().isValid()) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    return result;
}

QList<StoredSession> LocalJournal::sessions() const {
    return readSessions().value_or(QList<StoredSession>{});
}

QStringList LocalJournal::worktreePaths() const {
    QStringList result;
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("SELECT path FROM worktree_catalog ORDER BY path"))) {
        setError(query.lastError().text());
        return result;
    }
    while (query.next())
        result.append(query.value(0).toString());
    return result;
}

bool LocalJournal::hasOpenTurn(const QString& sessionId, qint64 generation) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT event_type FROM replay_events WHERE session_id = ? AND generation = ? "
        "AND event_type IN ('turn.started', 'turn.completed') ORDER BY cursor DESC LIMIT 1"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return query.next() && query.value(0).toString() == QStringLiteral("turn.started");
}

QHash<QString, QString> LocalJournal::worktreeRepositories() const {
    QHash<QString, QString> result;
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("SELECT path, repository_path FROM worktree_catalog"))) {
        setError(query.lastError().text());
        return result;
    }
    while (query.next())
        result.insert(query.value(0).toString(), query.value(1).toString());
    return result;
}

bool LocalJournal::rememberWorktreeRepository(const QString& worktreePath,
                                              const QString& repositoryPath) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("UPDATE worktree_catalog SET repository_path=? WHERE path=?"));
    query.addBindValue(repositoryPath);
    query.addBindValue(worktreePath);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

qint64 LocalJournal::appendEvent(const QString& sessionId, const qint64 generation,
                                 const QString& type, const QJsonObject& payload, QString* error) {
    return appendEventWithMutation(sessionId, generation, type, payload, error, {});
}

qint64 LocalJournal::appendEventWithMutation(const QString& sessionId, const qint64 generation,
                                             const QString& type, const QJsonObject& payload,
                                             QString* error, const std::function<bool()>& mutation,
                                             bool ownTransaction) {
    const QByteArray encoded = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (encoded.size() > protocol::maximumEventPayloadBytes) {
        const QJsonObject item = payload.value(QStringLiteral("item")).toObject();
        const QString itemType = payload.value(QStringLiteral("item_type"))
                                     .toString(item.value(QStringLiteral("type")).toString());
        const bool chunkable =
            (type == QStringLiteral("item.started") || type == QStringLiteral("item.completed")) &&
            itemType != QStringLiteral("commandExecution") && !eventItemId(payload).isEmpty() &&
            encoded.size() <= 32 * 1024 * 1024;
        if (!chunkable || !ownTransaction) {
            const QString message =
                QStringLiteral("Event payload exceeds the local journal limit and cannot be "
                               "represented as a bounded transcript item.");
            if (error != nullptr) {
                *error = message;
            }
            return -1;
        }
    }
    if (ownTransaction && !database_.transaction()) {
        if (error != nullptr) {
            *error = database_.lastError().text();
        }
        return -1;
    }
    QSqlQuery read(database_);
    read.prepare(QStringLiteral(
        "SELECT CASE WHEN typeof(last_cursor)='integer' THEN last_cursor END, "
        "CASE WHEN typeof(current_turn_sequence)='integer' THEN current_turn_sequence END, "
        "typeof(last_cursor), typeof(current_turn_sequence) FROM sessions "
        "WHERE id = ? AND generation = ?"));
    read.addBindValue(sessionId);
    read.addBindValue(generation);
    if (!read.exec() || !read.next()) {
        database_.rollback();
        if (error != nullptr) {
            *error = read.lastError().text().isEmpty() ? QStringLiteral("Unknown session.")
                                                       : read.lastError().text();
        }
        return -1;
    }
    const auto invalidCounters = [&] {
        database_.rollback();
        if (error != nullptr)
            *error = QStringLiteral("Journal counters are unsupported or exhausted.");
        return qint64(-1);
    };
    if (read.value(2).toString() != QStringLiteral("integer") ||
        read.value(3).toString() != QStringLiteral("integer"))
        return invalidCounters();
    const qint64 lastCursor = read.value(0).toLongLong();
    const qint64 previousTurn = read.value(1).toLongLong();
    const bool newTurn = type == QStringLiteral("user.message");
    if (lastCursor < 0 || lastCursor == std::numeric_limits<qint64>::max() || previousTurn < 0 ||
        (newTurn && previousTurn == std::numeric_limits<qint64>::max()))
        return invalidCounters();
    const qint64 cursor = lastCursor + 1;
    const qint64 turnSequence = previousTurn + (newTurn ? 1 : 0);
    if (encoded.size() > protocol::maximumEventPayloadBytes) {
        return appendLargeEvent(sessionId, generation, cursor, turnSequence, type, payload, encoded,
                                error);
    }
    QSqlQuery insert(database_);
    insert.prepare(QStringLiteral(
        "INSERT INTO events(session_id, generation, cursor, event_type, payload, sent_at, "
        "turn_sequence) VALUES(?, ?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(sessionId);
    insert.addBindValue(generation);
    insert.addBindValue(cursor);
    insert.addBindValue(type);
    insert.addBindValue(QString::fromUtf8(encoded));
    const QString sentAt = protocol::timestamp();
    insert.addBindValue(sentAt);
    insert.addBindValue(turnSequence);
    if (!insert.exec() || insert.numRowsAffected() != 1) {
        database_.rollback();
        if (error != nullptr) {
            *error = insert.lastError().isValid() ? insert.lastError().text()
                                                  : QStringLiteral("Journal event was not stored.");
        }
        return -1;
    }
    QString projectionError;
    if (!updateReplayProjection(sessionId, generation, cursor, turnSequence, type,
                                QString::fromUtf8(encoded), sentAt, &projectionError)) {
        database_.rollback();
        if (error != nullptr) {
            *error = projectionError;
        }
        return -1;
    }
    if (!updateActivityGroups(sessionId, generation, cursor, turnSequence, type, payload,
                              &projectionError)) {
        database_.rollback();
        if (error)
            *error = projectionError;
        return -1;
    }
    if (mutation && !mutation()) {
        database_.rollback();
        if (error != nullptr)
            *error = errorString();
        return -1;
    }
    QSqlQuery update(database_);
    update.prepare(QStringLiteral("UPDATE sessions SET last_cursor = ?, current_turn_sequence = ?, "
                                  "last_turn_started_at = CASE WHEN ? THEN ? "
                                  "ELSE last_turn_started_at END "
                                  "WHERE id = ? AND generation = ?"));
    update.addBindValue(cursor);
    update.addBindValue(turnSequence);
    update.addBindValue(type == QStringLiteral("user.message"));
    update.addBindValue(sentAt);
    update.addBindValue(sessionId);
    update.addBindValue(generation);
    if (!update.exec() || update.numRowsAffected() != 1) {
        database_.rollback();
        if (error != nullptr) {
            *error = update.lastError().isValid()
                         ? update.lastError().text()
                         : QStringLiteral("Session cursor was not stored.");
        }
        return -1;
    }
    if (ownTransaction && !database_.commit()) {
        const QString commitError = database_.lastError().text();
        database_.rollback();
        if (error != nullptr)
            *error = commitError;
        return -1;
    }
    return cursor;
}

QJsonArray LocalJournal::eventsAfter(const QString& sessionId, const qint64 generation,
                                     const qint64 cursor, const int limit) const {
    QJsonArray result;
    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral("SELECT cursor, turn_sequence, event_type, payload, sent_at FROM events "
                       "WHERE session_id = ? AND generation = ? AND cursor > ? "
                       "ORDER BY cursor LIMIT ?"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(cursor);
    query.addBindValue(qBound(1, limit, protocol::maximumReplayEvents));
    if (!query.exec()) {
        setError(query.lastError().text());
        return result;
    }
    qsizetype bytes = 0;
    while (query.next()) {
        const qsizetype eventBytes =
            replayEventBytes(sessionId, query.value(2).toString(), query.value(3).toString(),
                             query.value(4).toString());
        if (!result.isEmpty() && bytes + eventBytes > protocol::maximumReplayPayloadBytes) {
            break;
        }
        bytes += eventBytes;
        result.append(replayEvent(sessionId, generation, query));
    }
    return result;
}

EventReplayPage LocalJournal::compactedEventsAfter(const QString& sessionId,
                                                   const qint64 generation, const qint64 cursor,
                                                   const int limit) const {
    EventReplayPage page{{}, cursor};
    const int boundedLimit = qBound(1, limit, protocol::maximumReplayEvents);
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT cursor, turn_sequence, event_type, payload, sent_at FROM "
                                 "replay_events WHERE session_id = ? "
                                 "AND generation = ? AND cursor > ? ORDER BY cursor LIMIT ?"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(cursor);
    query.addBindValue(boundedLimit);
    if (!query.exec()) {
        setError(query.lastError().text());
        return page;
    }
    qsizetype bytes = 0;
    bool byteLimited = false;
    while (query.next()) {
        const qsizetype eventBytes =
            replayEventBytes(sessionId, query.value(2).toString(), query.value(3).toString(),
                             query.value(4).toString());
        if (!page.events.isEmpty() && (page.events.size() >= protocol::maximumReplayEvents ||
                                       bytes + eventBytes > protocol::maximumReplayPayloadBytes)) {
            byteLimited = true;
            break;
        }
        bytes += eventBytes;
        page.nextCursor = query.value(0).toLongLong();
        page.events.append(replayEvent(sessionId, generation, query));
    }
    if (!byteLimited && page.events.size() < boundedLimit) {
        QSqlQuery tail(database_);
        tail.prepare(
            QStringLiteral("SELECT last_cursor FROM sessions WHERE id = ? AND generation = ?"));
        tail.addBindValue(sessionId);
        tail.addBindValue(generation);
        if (tail.exec() && tail.next()) {
            page.nextCursor = tail.value(0).toLongLong();
        } else {
            setError(tail.lastError().text());
        }
    }
    return page;
}

TranscriptPage LocalJournal::transcriptPage(const QString& sessionId, const qint64 generation,
                                            const qint64 beforeTurnSequence, const int turnLimit,
                                            const qint64 beforeCursor) const {
    setError({});
    TranscriptPage page;
    QSqlQuery snapshot(database_);
    snapshot.prepare(
        QStringLiteral("SELECT last_cursor FROM sessions WHERE id = ? AND generation = ?"));
    snapshot.addBindValue(sessionId);
    snapshot.addBindValue(generation);
    if (!snapshot.exec() || !snapshot.next()) {
        setError(snapshot.lastError().text());
        return page;
    }
    page.snapshotCursor = snapshot.value(0).toLongLong();

    QSqlQuery turns(database_);
    QString statement = QStringLiteral(
        "SELECT DISTINCT turn_sequence FROM replay_events WHERE session_id = ? AND generation = ? "
        "AND turn_sequence > 0 ");
    if (beforeTurnSequence > 0) {
        statement += QStringLiteral("AND turn_sequence < ? ");
    }
    // before_cursor continues a page that the budget cut inside a turn: the rest of that turn
    // (events older than the cursor) is still selectable, then earlier turns.
    if (beforeCursor > 0) {
        statement += QStringLiteral("AND cursor < ? ");
    }
    statement += QStringLiteral("ORDER BY turn_sequence DESC LIMIT ?");
    turns.prepare(statement);
    turns.addBindValue(sessionId);
    turns.addBindValue(generation);
    if (beforeTurnSequence > 0) {
        turns.addBindValue(beforeTurnSequence);
    }
    if (beforeCursor > 0) {
        turns.addBindValue(beforeCursor);
    }
    turns.addBindValue(qBound(1, turnLimit, 20));
    if (!turns.exec()) {
        setError(turns.lastError().text());
        return page;
    }
    QList<qint64> sequences;
    while (turns.next()) {
        sequences.prepend(turns.value(0).toLongLong());
    }
    if (sequences.isEmpty()) {
        return page;
    }
    page.earliestTurnSequence = sequences.first();
    QSqlQuery more(database_);
    more.prepare(
        QStringLiteral("SELECT 1 FROM replay_events WHERE session_id = ? AND generation = ? "
                       "AND turn_sequence > 0 AND turn_sequence < ? LIMIT 1"));
    more.addBindValue(sessionId);
    more.addBindValue(generation);
    more.addBindValue(page.earliestTurnSequence);
    if (!more.exec()) {
        setError(more.lastError().text());
        return {};
    }
    page.hasMoreBefore = more.next();

    QStringList placeholders;
    placeholders.reserve(sequences.size());
    for (qsizetype index = 0; index < sequences.size(); ++index) {
        placeholders.append(QStringLiteral("?"));
    }
    QSqlQuery events(database_);
    events.prepare(
        QStringLiteral(
            "SELECT cursor, turn_sequence, event_type, payload, sent_at FROM replay_events "
            "WHERE session_id = ? AND generation = ? AND turn_sequence IN (%1) %2"
            "ORDER BY cursor DESC")
            .arg(placeholders.join(QLatin1Char(',')),
                 beforeCursor > 0 ? QStringLiteral("AND cursor < ? ") : QString()));
    events.addBindValue(sessionId);
    events.addBindValue(generation);
    for (const qint64 sequence : sequences) {
        events.addBindValue(sequence);
    }
    if (beforeCursor > 0) {
        events.addBindValue(beforeCursor);
    }
    if (!events.exec()) {
        setError(events.lastError().text());
        return {};
    }
    // Walk newest-first so a budget cut drops the oldest events: clients render the tail and page
    // backward for older history, so losing the newest output would be invisible and wrong.
    qsizetype bytes = 0;
    QList<QJsonObject> newestFirst;
    while (events.next()) {
        const qsizetype eventBytes =
            replayEventBytes(sessionId, events.value(2).toString(), events.value(3).toString(),
                             events.value(4).toString());
        if (!newestFirst.isEmpty() && (newestFirst.size() >= protocol::maximumReplayEvents ||
                                       bytes + eventBytes > protocol::maximumReplayPayloadBytes)) {
            page.truncated = true;
            break;
        }
        bytes += eventBytes;
        newestFirst.append(replayEvent(sessionId, generation, events));
    }
    for (auto it = newestFirst.crbegin(); it != newestFirst.crend(); ++it)
        page.events.append(*it);
    if (!newestFirst.isEmpty()) {
        page.firstCursor = newestFirst.constLast().value(QStringLiteral("cursor")).toInteger();
        // Continue history before the earliest turn that is at least partially present.
        page.earliestTurnSequence =
            newestFirst.constLast().value(QStringLiteral("turn_sequence")).toInteger();
        if (page.truncated)
            page.hasMoreBefore = true;
    }
    return page;
}

qint64 LocalJournal::earliestCompactedCursor(const QString& sessionId,
                                             const qint64 generation) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT first_cursor FROM replay_projection_state WHERE "
                                 "session_id = ? AND generation = ?"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec()) {
        setError(query.lastError().text());
        return -1;
    }
    // A session with no projected events yet has no projection row; nothing is compacted away.
    return query.next() ? query.value(0).toLongLong() : earliestCursor(sessionId, generation);
}

qint64 LocalJournal::earliestCursor(const QString& sessionId, const qint64 generation) const {
    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral("SELECT MIN(e.cursor), s.last_cursor FROM sessions s LEFT JOIN events e ON "
                       "e.session_id = s.id AND e.generation = s.generation "
                       "WHERE s.id = ? AND s.generation = ? GROUP BY s.id, s.last_cursor"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec() || !query.next()) {
        setError(query.lastError().text());
        return -1;
    }
    return query.value(0).isNull() ? query.value(1).toLongLong() + 1 : query.value(0).toLongLong();
}

QJsonObject LocalJournal::commandResult(const QString& messageId) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT result FROM command_results WHERE message_id = ?"));
    query.addBindValue(messageId);
    if (!query.exec()) {
        setError(query.lastError().text());
        return {};
    }
    return query.next() ? parseObject(query.value(0).toString()) : QJsonObject{};
}

bool LocalJournal::storeCommandResult(const QString& messageId, const QJsonObject& result) {
    return writeCommandResult(messageId, result, false);
}

bool LocalJournal::replaceCommandResult(const QString& messageId, const QJsonObject& result) {
    return writeCommandResult(messageId, result, true);
}

bool LocalJournal::writeCommandResult(const QString& messageId, const QJsonObject& result,
                                      const bool replace) {
    QSqlQuery query(database_);
    query.prepare(replace ? QStringLiteral("INSERT OR REPLACE INTO command_results(message_id, "
                                           "result, completed_at) VALUES(?, ?, ?)")
                          : QStringLiteral("INSERT OR IGNORE INTO command_results(message_id, "
                                           "result, completed_at) VALUES(?, ?, ?)"));
    query.addBindValue(messageId);
    query.addBindValue(compact(result));
    query.addBindValue(protocol::timestamp());
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    if (++commandResultsSincePrune_ >= 256 && !pruneRetention())
        qWarning("Journal retention pass failed: %s", qUtf8Printable(error_));
    return true;
}

void LocalJournal::setError(const QString& error) const { error_ = error; }

} // namespace ketplus::daemon
