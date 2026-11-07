#include "protocol/DaemonProtocol.h"
#include "storage/LocalJournal.h"
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {
namespace {
QJsonObject parseObject(const QString& value) {
    return QJsonDocument::fromJson(value.toUtf8()).object();
}
QString compact(const QJsonObject& value) {
    return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}

// Mark only the envelopes in this upload. A prior account's high-water cursor
// cannot authorize deletion of regenerated ciphertext. One transaction touches
// at most 100 cached rows and 1 MiB of their stored column bytes.
bool markUpload(QSqlDatabase db, const QJsonArray& envelopes, bool acknowledged,
                qint64 acceptedThrough, QString* error, const QString& scope = {},
                const QString& sessionId = {}, qint64 generationId = 0) {
    const auto fail = [&](const QString& detail) {
        *error = detail;
        return false;
    };
    if (envelopes.size() > 100 || acceptedThrough < 0)
        return fail("Invalid cloud upload receipt bounds.");
    qint64 progressBytes = 0;
    if (acknowledged) {
        if (scope.isEmpty() || scope.size() > 4096 || sessionId.isEmpty() ||
            sessionId.size() > 256 || generationId < 1 || acceptedThrough < 1 ||
            envelopes.isEmpty())
            return fail("Invalid scoped cloud upload progress.");
        progressBytes = scope.toUtf8().size() + sessionId.toUtf8().size() + 16;
        qint64 highest = 0;
        for (const auto& value : envelopes) {
            const auto sent = value.toObject();
            if (sent.value("sessionId").toString() != sessionId ||
                sent.value("sessionGeneration").toInteger(-1) != generationId)
                return fail("Cloud upload receipt scope does not match its batch.");
            highest = qMax(highest, sent.value("cursor").toInteger(-1));
        }
        if (acceptedThrough > highest)
            return fail("Cloud upload receipt exceeds its batch.");
    }
    QSqlQuery timeout(db);
    if (!timeout.exec("PRAGMA busy_timeout") || !timeout.next())
        return fail(timeout.lastError().text());
    const int oldTimeout = timeout.value(0).toInt();
    timeout.finish();
    if (!timeout.exec("PRAGMA busy_timeout=0"))
        return fail(timeout.lastError().text());
    timeout.finish();
    bool transaction = false;
    const auto restore = qScopeGuard([&] {
        if (transaction)
            db.rollback();
        QSqlQuery q(db);
        q.exec(QString("PRAGMA busy_timeout=%1").arg(oldTimeout));
    });
    if (!db.transaction())
        return fail(db.lastError().text());
    transaction = true;
    qint64 totalBytes = progressBytes;
    for (const auto& value : envelopes) {
        const auto sent = value.toObject();
        const auto session = sent.value("sessionId").toString();
        const qint64 generation = sent.value("sessionGeneration").toInteger(-1);
        const qint64 cursor = sent.value("cursor").toInteger(-1);
        if (session.isEmpty() || session.size() > 256 || session.toUtf8().size() > 256 ||
            generation < 1 || cursor < 1 || !sent.value("content").isObject() ||
            sent.value("content").toObject().isEmpty())
            return fail("Invalid cloud upload envelope identity.");
        if (acknowledged && cursor > acceptedThrough)
            continue;
        QSqlQuery q(db);
        q.prepare("SELECT rowid,CASE WHEN typeof(event)='text' AND typeof(created_at)='text' "
                  "AND typeof(upload_acked)='integer' AND upload_acked IN (0,1) THEN "
                  "octet_length(event)+octet_length(created_at)+octet_length(session_id)+32 "
                  "ELSE -1 END,upload_acked FROM cloud_events "
                  "WHERE session_id=? AND generation=? AND cursor=?");
        q.addBindValue(session);
        q.addBindValue(generation);
        q.addBindValue(cursor);
        if (!q.exec())
            return fail(q.lastError().text());
        if (!q.next()) {
            if (q.lastError().isValid())
                return fail(q.lastError().text());
            if (acknowledged)
                continue;
            return fail("Cloud upload cache changed before dispatch.");
        }
        const auto row = q.value(0).toLongLong();
        const auto bytes = q.value(1).toLongLong();
        const int previous = q.value(2).toInt();
        q.finish();
        if (bytes < 0 || bytes > 1024 * 1024 - totalBytes)
            return fail("Cloud upload cache exceeds its receipt work budget.");
        totalBytes += bytes;
        q.prepare("SELECT event FROM cloud_events WHERE rowid=?");
        q.addBindValue(row);
        if (!q.exec() || !q.next())
            return fail("Cloud upload cache could not be verified.");
        const auto cached = parseObject(q.value(0).toString());
        q.finish();
        const bool matches = cached.value("sessionId") == sent.value("sessionId") &&
                             cached.value("sessionGeneration") == sent.value("sessionGeneration") &&
                             cached.value("cursor") == sent.value("cursor") &&
                             cached.value("content") == sent.value("content");
        if (!matches) {
            if (acknowledged)
                continue; // A newer ciphertext must retain its own receipt.
            return fail("Cloud upload cache changed before dispatch.");
        }
        if (previous == int(acknowledged))
            continue;
        q.prepare("UPDATE cloud_events SET upload_acked=? WHERE rowid=?");
        q.addBindValue(int(acknowledged));
        q.addBindValue(row);
        if (!q.exec())
            return fail(q.lastError().text());
        if (q.numRowsAffected() != 1)
            return fail("Cloud upload cache receipt was not stored.");
    }
    if (acknowledged) {
        QSqlQuery progress(db);
        progress.prepare(
            "INSERT INTO cloud_upload_progress(scope,session_id,generation,cursor) "
            "VALUES(?,?,?,?) ON CONFLICT(scope,session_id,generation) DO UPDATE SET "
            "cursor=MAX(cursor,excluded.cursor) WHERE typeof(cursor)='integer' AND cursor>=0");
        progress.addBindValue(scope);
        progress.addBindValue(sessionId);
        progress.addBindValue(generationId);
        progress.addBindValue(acceptedThrough);
        if (!progress.exec())
            return fail(progress.lastError().text());
        if (progress.numRowsAffected() != 1)
            return fail("Scoped cloud upload progress was not stored.");
    }
    if (!db.commit())
        return fail(db.lastError().text());
    transaction = false;
    return true;
}

} // namespace
namespace detail {
QString cloudCacheInvalidationQuery() {
    return "SELECT rowid,CASE WHEN typeof(event)='text' AND typeof(created_at)='text' "
           "AND typeof(session_id)='text' AND typeof(generation)='integer' "
           "AND typeof(cursor)='integer' AND typeof(projection_policy_version)='integer' "
           "THEN octet_length(event)+octet_length(created_at)+octet_length(session_id)+24 "
           "ELSE -1 END FROM cloud_events WHERE session_id=? AND generation=? AND cursor=?";
}
} // namespace detail

QJsonObject LocalJournal::cloudEvent(const QString& sessionId, const qint64 generation,
                                     const qint64 cursor) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT event FROM cloud_events WHERE session_id = ? AND generation = ? AND cursor = ?"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(cursor);
    if (!query.exec()) {
        setError(query.lastError().text());
        return {};
    }
    return query.next() ? parseObject(query.value(0).toString()) : QJsonObject{};
}

int LocalJournal::cloudEventProjectionPolicyVersion(const QString& sessionId,
                                                    const qint64 generation,
                                                    const qint64 cursor) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT projection_policy_version FROM cloud_events "
                                 "WHERE session_id = ? AND generation = ? AND cursor = ?"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(cursor);
    if (!query.exec()) {
        setError(query.lastError().text());
        return -1;
    }
    return query.next() ? query.value(0).toInt() : -1;
}

bool LocalJournal::storeCloudEvent(const QString& sessionId, const qint64 generation,
                                   const qint64 cursor, const QJsonObject& event,
                                   const int projectionPolicyVersion) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO cloud_events(session_id, generation, cursor, event, created_at, "
        "projection_policy_version) VALUES(?, ?, ?, ?, ?, ?)"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(cursor);
    query.addBindValue(compact(event));
    query.addBindValue(protocol::timestamp());
    query.addBindValue(projectionPolicyVersion);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    const QJsonObject stored = cloudEvent(sessionId, generation, cursor);
    if (stored != event || cloudEventProjectionPolicyVersion(sessionId, generation, cursor) !=
                               projectionPolicyVersion) {
        setError(QStringLiteral("Cloud event cache conflicts with the local journal."));
        return false;
    }
    return true;
}

bool LocalJournal::prepareCloudEventUpload(const QJsonArray& envelopes) {
    QString error;
    if (!markUpload(database_, envelopes, false, 0, &error)) {
        setError(error);
        return false;
    }
    return true;
}

bool LocalJournal::acknowledgeCloudEventUpload(const QString& scope, const QString& sessionId,
                                               qint64 generation, const QJsonArray& envelopes,
                                               qint64 acceptedThrough) {
    QString error;
    if (!markUpload(database_, envelopes, true, acceptedThrough, &error, scope, sessionId,
                    generation)) {
        setError(error);
        return false;
    }
    return true;
}

qint64 LocalJournal::cloudUploadCursor(const QString& scope, const QString& sessionId,
                                       qint64 generation) const {
    QSqlQuery query(database_);
    query.prepare("SELECT CASE WHEN typeof(cursor)='integer' AND cursor>=0 THEN cursor ELSE -1 END "
                  "FROM cloud_upload_progress WHERE scope=? AND session_id=? AND generation=?");
    query.addBindValue(scope);
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec()) {
        setError(query.lastError().text());
        return -1;
    }
    if (!query.next()) {
        if (query.lastError().isValid()) {
            setError(query.lastError().text());
            return -1;
        }
        return 0;
    }
    const auto cursor = query.value(0).toLongLong();
    if (cursor < 0)
        setError("Scoped cloud upload progress requires recovery.");
    return cursor;
}

bool LocalJournal::discardCloudEvent(const QString& sessionId, qint64 generation, qint64 cursor) {
    if (sessionId.isEmpty() || sessionId.toUtf8().size() > 256 || generation < 1 || cursor < 1) {
        setError("Invalid cloud cache identity.");
        return false;
    }
    QSqlQuery timeout(database_);
    if (!timeout.exec("PRAGMA busy_timeout") || !timeout.next()) {
        setError(timeout.lastError().text());
        return false;
    }
    const int oldTimeout = timeout.value(0).toInt();
    timeout.finish();
    if (!timeout.exec("PRAGMA busy_timeout=0")) {
        setError(timeout.lastError().text());
        return false;
    }
    timeout.finish();
    bool transaction = false;
    const auto restore = qScopeGuard([&] {
        if (transaction)
            database_.rollback();
        QSqlQuery q(database_);
        q.exec(QString("PRAGMA busy_timeout=%1").arg(oldTimeout));
    });
    if (!database_.transaction()) {
        setError(database_.lastError().text());
        return false;
    }
    transaction = true;
    QSqlQuery query(database_);
    query.prepare(detail::cloudCacheInvalidationQuery());
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(cursor);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    if (query.next()) {
        const qint64 row = query.value(0).toLongLong();
        const qint64 bytes = query.value(1).toLongLong();
        query.finish();
        if (bytes < 0 || bytes > 1024 * 1024) {
            setError("Cloud cache entry requires bounded recovery before upload.");
            return false;
        }
        query.prepare("DELETE FROM cloud_events WHERE rowid=?");
        query.addBindValue(row);
        if (!query.exec()) {
            setError(query.lastError().text());
            return false;
        }
        if (query.numRowsAffected() != 1) {
            setError("Cloud cache entry was not discarded.");
            return false;
        }
    } else if (query.lastError().isValid()) {
        setError(query.lastError().text());
        return false;
    }
    query.finish();
    if (!database_.commit()) {
        setError(database_.lastError().text());
        return false;
    }
    transaction = false;
    return true;
}
} // namespace ketplus::daemon
