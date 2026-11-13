#include "storage/LocalJournal.h"
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {
namespace {
const QString columns =
    QStringLiteral("session_id,owner,request_id,worktree_path,options,expires_at,state");
SessionPreparation readPreparation(const QSqlQuery& query) {
    return {query.value(0).toString(),
            query.value(1).toString(),
            query.value(2).toString(),
            query.value(3).toString(),
            QJsonDocument::fromJson(query.value(4).toByteArray()).object(),
            query.value(5).toLongLong(),
            query.value(6).toString()};
}
} // namespace

bool LocalJournal::prepareSession(SessionPreparation& preparation, QString* error) {
    const auto fail = [&](const QString& code) {
        setError(code);
        if (error)
            *error = code;
        return false;
    };
    if (preparation.sessionId.isEmpty() || preparation.sessionId.size() > 128 ||
        preparation.owner.isEmpty() || preparation.owner.size() > 256 ||
        preparation.requestId.isEmpty() || preparation.requestId.size() > 128 ||
        preparation.worktreePath.isEmpty() || preparation.worktreePath.size() > 4096 ||
        preparation.expiresAt <= 0 || preparation.state != "prepared" ||
        QJsonDocument(preparation.options).toJson(QJsonDocument::Compact).size() > 8 * 1024)
        return fail("invalid_preparation");
    if (!database_.transaction())
        return fail("preparation_journal_error");
    bool transaction = true;
    const auto rollback = qScopeGuard([&] {
        if (transaction)
            database_.rollback();
    });
    QSqlQuery existing(database_);
    existing.prepare("SELECT " + columns +
                     " FROM session_preparations WHERE owner=? AND request_id=?");
    existing.addBindValue(preparation.owner);
    existing.addBindValue(preparation.requestId);
    if (!existing.exec())
        return fail("preparation_journal_error");
    if (existing.next()) {
        const auto original = readPreparation(existing);
        existing.finish();
        if (original.worktreePath != preparation.worktreePath ||
            original.options != preparation.options)
            return fail("preparation_request_conflict");
        preparation = original; // Includes terminal receipts: retry never allocates again.
    } else {
        existing.finish();
        QSqlQuery capacity(database_);
        capacity.prepare("SELECT COUNT(*), "
                         "COALESCE(SUM(state='prepared'),0), "
                         "COALESCE(SUM(state='prepared' AND owner=?),0) "
                         "FROM session_preparations");
        capacity.addBindValue(preparation.owner);
        if (!capacity.exec() || !capacity.next())
            return fail("preparation_journal_error");
        if (capacity.value(0).toInt() >= 32768 || capacity.value(1).toInt() >= 2048 ||
            capacity.value(2).toInt() >= 128)
            return fail("preparation_capacity");
        capacity.finish();
        QSqlQuery insert(database_);
        insert.prepare("INSERT INTO session_preparations(" + columns + ") VALUES(?,?,?,?,?,?,?)");
        for (const auto& value :
             {preparation.sessionId, preparation.owner, preparation.requestId,
              preparation.worktreePath,
              QString::fromUtf8(QJsonDocument(preparation.options).toJson(QJsonDocument::Compact))})
            insert.addBindValue(value);
        insert.addBindValue(preparation.expiresAt);
        insert.addBindValue(preparation.state);
        if (!insert.exec())
            return fail("preparation_journal_error");
    }
    if (!database_.commit())
        return fail("preparation_journal_error");
    transaction = false;
    return true;
}

std::optional<SessionPreparation> LocalJournal::sessionPreparation(const QString& sessionId) const {
    QSqlQuery query(database_);
    query.prepare("SELECT " + columns + " FROM session_preparations WHERE session_id=?");
    query.addBindValue(sessionId);
    if (!query.exec() || !query.next())
        return std::nullopt;
    return readPreparation(query);
}

QList<SessionPreparation> LocalJournal::sessionPreparations() const {
    QList<SessionPreparation> result;
    QSqlQuery query(database_);
    if (!query.exec(
            "SELECT " + columns +
            " FROM session_preparations WHERE state='prepared' ORDER BY expires_at LIMIT 2048")) {
        setError(query.lastError().text());
        return result;
    }
    while (query.next())
        result.append(readPreparation(query));
    return result;
}

bool LocalJournal::pruneSessionPreparations(const qint64 nowMs, const int limit) {
    if (nowMs <= 0 || limit < 1 || limit > 128)
        return false;
    QSqlQuery query(database_);
    query.prepare("DELETE FROM session_preparations WHERE session_id IN "
                  "(SELECT session_id FROM session_preparations "
                  "WHERE state IN ('activated','discarded','expired') AND expires_at<=? "
                  "ORDER BY expires_at LIMIT ?)");
    query.addBindValue(nowMs - qint64(commandResultRetentionDays) * 24 * 60 * 60 * 1000);
    query.addBindValue(limit);
    if (query.exec())
        return true;
    setError(query.lastError().text());
    return false;
}

bool LocalJournal::activatePreparedSession(const StoredSession& session, const QString& owner,
                                           const qint64 nowMs, QString* error) {
    const auto fail = [&](const QString& code) {
        setError(code);
        if (error)
            *error = code;
        return false;
    };
    if (!database_.transaction())
        return fail("preparation_journal_error");
    bool transaction = true;
    const auto rollback = qScopeGuard([&] {
        if (transaction)
            database_.rollback();
    });
    const auto preparation = sessionPreparation(session.id);
    if (!preparation || preparation->owner != owner || preparation->state != "prepared")
        return fail("preparation_not_found");
    if (preparation->expiresAt <= nowMs)
        return fail("preparation_expired");
    if (session.generation != 1 || session.lastCursor != 0 || session.currentTurnSequence != 0 ||
        !session.threadId.isEmpty() || !session.turnId.isEmpty() ||
        session.worktreePath != preparation->worktreePath)
        return fail("preparation_scope_changed");
    QSqlQuery existing(database_);
    existing.prepare("SELECT 1 FROM sessions WHERE id=?");
    existing.addBindValue(session.id);
    if (!existing.exec())
        return fail("preparation_journal_error");
    if (existing.next())
        return fail("preparation_consumed");
    existing.finish();
    if (!upsertSession(session))
        return fail("preparation_journal_error");
    QSqlQuery activate(database_);
    activate.prepare("UPDATE session_preparations SET state='activated' WHERE session_id=? AND "
                     "state='prepared'");
    activate.addBindValue(session.id);
    if (!activate.exec() || activate.numRowsAffected() != 1 || !database_.commit())
        return fail("preparation_journal_error");
    transaction = false;
    return true;
}

bool LocalJournal::discardPreparedSession(const QString& sessionId, const QString& owner,
                                          const qint64 nowMs, QString* error) {
    const auto fail = [&](const QString& code) {
        setError(code);
        if (error)
            *error = code;
        return false;
    };
    if (!database_.transaction())
        return fail("preparation_journal_error");
    bool transaction = true;
    const auto rollback = qScopeGuard([&] {
        if (transaction)
            database_.rollback();
    });
    const auto preparation = sessionPreparation(sessionId);
    if (!preparation || preparation->owner != owner)
        return fail("preparation_not_found");
    if (preparation->state == "discarded" || preparation->state == "expired") {
        database_.rollback();
        transaction = false;
        return true;
    }
    if (preparation->state != "prepared")
        return fail("preparation_consumed");
    QSqlQuery live(database_);
    live.prepare("SELECT 1 FROM sessions WHERE id=?");
    live.addBindValue(sessionId);
    if (!live.exec())
        return fail("preparation_journal_error");
    if (live.next())
        return fail("preparation_consumed");
    live.finish();
    for (const auto& table : {"staged_attachment_chunks", "staged_attachments"}) {
        QSqlQuery remove(database_);
        remove.prepare(
            QStringLiteral("DELETE FROM %1 WHERE session_id=? AND generation=1").arg(table));
        remove.addBindValue(sessionId);
        if (!remove.exec())
            return fail("preparation_journal_error");
    }
    QSqlQuery discard(database_);
    discard.prepare(
        "UPDATE session_preparations SET state=? WHERE session_id=? AND state='prepared'");
    discard.addBindValue(preparation->expiresAt <= nowMs ? "expired" : "discarded");
    discard.addBindValue(sessionId);
    if (!discard.exec() || discard.numRowsAffected() != 1 || !database_.commit())
        return fail("preparation_journal_error");
    transaction = false;
    return true;
}
} // namespace ketplus::daemon
