// ADR 0127 §7a: owner notices. A completion tells the owner that a job ended; a notice tells it
// that a job it still owns needs something it cannot get by waiting: cleanup only the user can
// acknowledge, or a queue position that will not move by itself soon. Both reach the owner
// through the same durable acceptance, wake and fences.
#include "storage/BackgroundTaskLedger.h"
#include "storage/RemoteJobRows.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {
namespace {
const QString attentionEvent = QStringLiteral("needs_attention");
const QString queuedEvent = QStringLiteral("queued");

QJsonObject failure(const QString& code, const QString& message) {
    return {{"ok", false}, {"error_code", code}, {"error_message", message}};
}

// Inside the caller's transaction. A notice already recorded under `eventId` is kept as is.
QJsonObject insertNotice(QSqlDatabase database, const QString& eventId, const QString& kind,
                         const QJsonObject& record, const QJsonObject& payload,
                         const QString& suppressed) {
    QSqlQuery query(database);
    query.prepare("INSERT OR IGNORE INTO managed_job_notices(event_id,task_id,kind,"
                  "owner_session_id,owner_generation,payload,status,reason) "
                  "VALUES(?,?,?,?,?,?,?,?)");
    query.addBindValue(eventId);
    query.addBindValue(record.value("task_id").toString());
    query.addBindValue(kind);
    query.addBindValue(record.value("owner_session_id").toString());
    query.addBindValue(record.value("owner_generation").toInteger());
    query.addBindValue(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    query.addBindValue(suppressed.isEmpty() ? QStringLiteral("pending") : "suppressed");
    query.addBindValue(suppressed.isEmpty() ? QVariant{} : QVariant(suppressed));
    if (!query.exec())
        return failure("storage_error", query.lastError().text());
    return {{"ok", true}};
}
} // namespace

QJsonObject BackgroundTaskLedger::noticeAttention(const QString& id, const QJsonObject& record,
                                                  const QString& previousState) {
    // One notice per entry into needs_attention, keyed by the revision that entered it. A
    // later update of the same incident (its executor stopped, a cleanup retry) adds none.
    if (previousState == attentionEvent)
        return {{"ok", true}};
    QJsonObject payload{{"event", attentionEvent},
                        {"task_id", id},
                        {"title", record.value("title")},
                        {"state", attentionEvent},
                        {"reason", "cleanup_unverified"},
                        {"cleanup_status", "unverified"},
                        {"manual_guidance", record.value("manual_guidance")},
                        {"output_reference", id}};
    if (record.contains("exit_code"))
        payload.insert("exit_code", record.value("exit_code"));
    // A job its owner or the user cancelled wakes no one, as its completion would not (§7).
    const QString suppressed =
        record.value("terminal_target") == "cancelled" ? QStringLiteral("cancelled") : QString();
    return insertNotice(database_,
                        id + ":attention:" + QString::number(record.value("revision").toInteger()),
                        attentionEvent, record, payload, suppressed);
}

QJsonObject BackgroundTaskLedger::supersedeNotices(const QString& id) {
    QSqlQuery query(database_);
    query.prepare("UPDATE managed_job_wakes SET state='suppressed' WHERE state='accepted' AND "
                  "event_id IN (SELECT event_id FROM managed_job_notices WHERE task_id=?)");
    query.addBindValue(id);
    if (!query.exec())
        return failure("storage_error", query.lastError().text());
    query.prepare("UPDATE managed_job_notices SET status='suppressed',reason='superseded' "
                  "WHERE task_id=? AND status='pending'");
    query.addBindValue(id);
    if (!query.exec())
        return failure("storage_error", query.lastError().text());
    return {{"ok", true}};
}

QJsonObject BackgroundTaskLedger::noticeQueued(const QString& id, const qint64 nowMs) {
    if (!database_.transaction())
        return failure("storage_error", database_.lastError().text());
    const auto fail = [&](const QString& code, const QString& message) {
        database_.rollback();
        return failure(code, message);
    };
    const auto done = [&](const bool queued, const QString& reason = {}) {
        if (!database_.commit())
            return fail("storage_error", database_.lastError().text());
        return QJsonObject{{"ok", true}, {"queued", queued}, {"queue_reason", reason}};
    };
    const auto record = task(id);
    if (record.contains("error_code"))
        return fail(record.value("error_code").toString(),
                    record.value("error_message").toString());
    const qint64 createdAt = record.value("created_at").toInteger();
    // A permission request is the user's to answer and shown to them; the clock that matters
    // to the owner is the queue's, once the job may run.
    if (record.value("state") != queuedEvent || !record.value("permission_approved").toBool() ||
        nowMs < createdAt + queuedNoticeAfterMs(record.value("timeout_ms").toInteger()))
        return done(false);
    const QString eventId = id + ":queued";
    QSqlQuery query(database_);
    query.prepare("SELECT json_extract(payload,'$.queue_reason') FROM managed_job_notices "
                  "WHERE event_id=?");
    query.addBindValue(eventId);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (query.next()) {
        const QString reason = query.value(0).toString();
        query.finish();
        return done(true, reason);
    }
    query.finish();

    QString reason;
    QJsonArray holders;
    if (remote_job_rows::phaseOf(database_, id).value_or(remote_job_rows::Phase::Local) !=
        remote_job_rows::Phase::Local) {
        reason = QStringLiteral("waiting_for_runner");
    } else {
        query.prepare("SELECT h.task_id,json_extract(h.record,'$.title'),h.state FROM "
                      "managed_jobs j JOIN managed_jobs h ON h.root_session_id=j.root_session_id "
                      "AND h.root_generation=j.root_generation AND h.holds_slot=1 "
                      "WHERE j.task_id=? ORDER BY h.sequence");
        query.addBindValue(id);
        if (!query.exec())
            return fail("storage_error", query.lastError().text());
        while (query.next())
            holders.append(QJsonObject{{"task_id", query.value(0).toString()},
                                       {"title", query.value(1).toString()},
                                       {"state", query.value(2).toString()}});
        query.finish();
        reason = holders.size() >= rootSlotLimit ? QStringLiteral("root_slot_limit")
                                                 : QStringLiteral("awaiting_admission");
    }
    QJsonObject payload{
        {"event", queuedEvent},           {"task_id", id},
        {"title", record.value("title")}, {"state", queuedEvent},
        {"queued_ms", nowMs - createdAt}, {"timeout_ms", record.value("timeout_ms")},
        {"queue_reason", reason}};
    if (reason == QStringLiteral("root_slot_limit")) {
        payload.insert("slot_limit", rootSlotLimit);
        payload.insert("slot_holders", holders);
    }
    const auto inserted = insertNotice(database_, eventId, queuedEvent, record, payload, {});
    if (!inserted.value("ok").toBool()) {
        database_.rollback();
        return inserted;
    }
    return done(true, reason);
}

QJsonObject BackgroundTaskLedger::noticePage(const qint64 after) const {
    QSqlQuery query(database_);
    query.prepare(
        "SELECT rowid,event_id,owner_session_id,owner_generation,payload FROM "
        "managed_job_notices WHERE status='pending' AND rowid>? ORDER BY rowid LIMIT 100");
    query.addBindValue(after);
    if (!query.exec())
        return failure("storage_error", query.lastError().text());
    QJsonArray events;
    qint64 next = 0;
    while (query.next()) {
        next = query.value(0).toLongLong();
        events.append(QJsonObject{
            {"event_id", query.value(1).toString()},
            {"owner_session_id", query.value(2).toString()},
            {"owner_generation", query.value(3).toLongLong()},
            {"turn_record", QJsonDocument::fromJson(query.value(4).toByteArray()).object()}});
    }
    return {{"ok", true}, {"events", events}, {"next", events.size() == 100 ? next : 0}};
}

bool BackgroundTaskLedger::noticeCurrent(const QJsonObject& turnRecord) const {
    const QString event = turnRecord.value("event").toString();
    if (event != attentionEvent && event != queuedEvent)
        return true; // A completion describes a terminal job, which cannot move on.
    const auto record = task(turnRecord.value("task_id").toString());
    return !record.contains("error_code") && record.value("state").toString() == event;
}

} // namespace ketplus::daemon
