#include "storage/LocalJournal.h"

#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

namespace ketplus::daemon {
namespace {
QString itemId(const QJsonObject& payload) {
    return payload.value("item_id").toString(
        payload.value("item").toObject().value("id").toString());
}

QString family(const QJsonObject& payload) {
    const auto item = payload.value("item").toObject();
    const auto type = item.value("type").toString(payload.value("item_type").toString());
    if (type == "commandExecution")
        return "command";
    if (type == "fileChange")
        return "edit";
    if (type == "webSearch")
        return "search";
    if (type == "mcpToolCall" || type == "dynamicToolCall") {
        const auto tool = item.value("tool").toString();
        // Evidence and unknown MCP results stay outside groups, including while running.
        if (tool == "browser_capture" || tool == "visual_compare" ||
            item.value("arguments").toObject().value("includeScreenshot").toBool())
            return {};
        if (item.value("server").toString().contains("ketplus_browser") &&
            tool.startsWith("browser_"))
            return "browser";
        const auto operation = tool.toLower().section(QStringLiteral("__"), -1);
        if (operation == "read_file" || operation == "read")
            return "read";
        if (operation == "search_files" || operation == "glob" || operation == "search_code" ||
            operation == "grep")
            return "search";
    }
    return {};
}

QJsonObject failure(const QString& code, const QString& message) {
    return {{"ok", false}, {"error_code", code}, {"error_message", message}};
}
} // namespace

bool LocalJournal::initializeActivityGroups() {
    QSqlQuery query(database_);
    for (const auto& sql :
         {"CREATE TABLE IF NOT EXISTS activity_items (session_id TEXT NOT NULL, generation INTEGER "
          "NOT NULL, turn_sequence INTEGER NOT NULL, item_id TEXT NOT NULL, first_cursor INTEGER "
          "NOT NULL, last_cursor INTEGER NOT NULL, group_cursor INTEGER NOT NULL, family TEXT NOT "
          "NULL, running INTEGER NOT NULL, failed INTEGER NOT NULL, PRIMARY "
          "KEY(session_id,generation,turn_sequence,item_id))",
          "CREATE INDEX IF NOT EXISTS activity_items_group ON "
          "activity_items(session_id,generation,group_cursor,first_cursor)",
          "CREATE TABLE IF NOT EXISTS activity_summaries (session_id TEXT NOT NULL, generation "
          "INTEGER NOT NULL, group_cursor INTEGER NOT NULL, family TEXT NOT NULL, count INTEGER "
          "NOT NULL, running INTEGER NOT NULL, failed INTEGER NOT NULL, last_cursor INTEGER NOT "
          "NULL, turn_sequence INTEGER NOT NULL, PRIMARY "
          "KEY(session_id,generation,group_cursor,family))",
          "CREATE TABLE IF NOT EXISTS activity_heads (session_id TEXT NOT NULL, generation INTEGER "
          "NOT NULL, turn_sequence INTEGER NOT NULL, group_cursor INTEGER NOT NULL, indexed_cursor "
          "INTEGER NOT NULL, PRIMARY KEY(session_id,generation))"}) {
        if (!query.exec(QString::fromLatin1(sql))) {
            setError(query.lastError().text());
            return false;
        }
    }
    // Upgrade old journals once, streaming retained events rather than materializing history.
    if (!database_.transaction()) {
        setError(database_.lastError().text());
        return false;
    }
    QSqlQuery sessions(database_);
    if (!sessions.exec(QStringLiteral(
            "SELECT s.id,s.generation,COALESCE(h.indexed_cursor,0) FROM sessions s LEFT JOIN "
            "activity_heads h ON h.session_id=s.id AND h.generation=s.generation WHERE "
            "s.last_cursor>COALESCE(h.indexed_cursor,0)"))) {
        setError(sessions.lastError().text());
        database_.rollback();
        return false;
    }
    QList<QVariantList> pending;
    while (sessions.next())
        pending.append({sessions.value(0), sessions.value(1), sessions.value(2)});
    sessions.finish();
    for (const auto& session : pending) {
        QSqlQuery rows(database_);
        rows.prepare("SELECT cursor,turn_sequence,event_type,payload FROM replay_events WHERE "
                     "session_id=? AND generation=? AND cursor>? ORDER BY cursor");
        for (const auto& value : session)
            rows.addBindValue(value);
        if (!rows.exec()) {
            setError(rows.lastError().text());
            database_.rollback();
            return false;
        }
        while (rows.next()) {
            QString error;
            if (!updateActivityGroups(
                    session.at(0).toString(), session.at(1).toLongLong(),
                    rows.value(0).toLongLong(), rows.value(1).toLongLong(),
                    rows.value(2).toString(),
                    QJsonDocument::fromJson(rows.value(3).toString().toUtf8()).object(), &error)) {
                setError(error);
                database_.rollback();
                return false;
            }
        }
    }
    if (!database_.commit()) {
        setError(database_.lastError().text());
        return false;
    }
    return true;
}

bool LocalJournal::updateActivityGroups(const QString& session, qint64 generation, qint64 cursor,
                                        qint64 turn, const QString& type,
                                        const QJsonObject& payload, QString* error) {
    const auto exec = [&](QSqlQuery& query, bool oneRow = false) {
        if (query.exec() && (!oneRow || query.numRowsAffected() == 1))
            return true;
        if (error)
            *error = query.lastError().isValid()
                         ? query.lastError().text()
                         : QStringLiteral("Activity projection row was not stored.");
        return false;
    };
    qint64 head = 0;
    QSqlQuery current(database_);
    current.prepare("SELECT group_cursor,turn_sequence FROM activity_heads WHERE session_id=? AND "
                    "generation=?");
    current.addBindValue(session);
    current.addBindValue(generation);
    if (!exec(current))
        return false;
    if (current.next() && current.value(1).toLongLong() == turn)
        head = current.value(0).toLongLong();

    const bool lifecycle = type == "item.started" || type == "item.completed";
    const QString id = itemId(payload);
    if ((lifecycle || type == "command.output_delta") && !id.isEmpty()) {
        QSqlQuery existing(database_);
        existing.prepare(
            "SELECT group_cursor,family,running,failed FROM activity_items WHERE session_id=? AND "
            "generation=? AND turn_sequence=? AND item_id=?");
        existing.addBindValue(session);
        existing.addBindValue(generation);
        existing.addBindValue(turn);
        existing.addBindValue(id);
        if (!exec(existing))
            return false;
        const bool known = existing.next();
        const auto item = payload.value("item").toObject();
        QString kind = known       ? existing.value(1).toString()
                       : lifecycle ? family(payload)
                                   : QString{};
        const qint64 group = known            ? existing.value(0).toLongLong()
                             : kind.isEmpty() ? 0
                             : head > 0       ? head
                                              : cursor;
        if (!known)
            head = group;
        const int wasRunning = known ? existing.value(2).toInt() : 0;
        const int wasFailed = known ? existing.value(3).toInt() : 0;
        const int running = lifecycle ? type == "item.started" : wasRunning;
        const int failed = lifecycle ? item.value("status") == "failed" ||
                                           item.value("exitCode").toInt() != 0 ||
                                           item.value("result").toObject().value("isError").toBool()
                                     : wasFailed;
        if (lifecycle) {
            QSqlQuery upsert(database_);
            upsert.prepare(
                "INSERT INTO activity_items VALUES(?,?,?,?,?,?,?,?,?,?) ON "
                "CONFLICT(session_id,generation,turn_sequence,item_id) DO UPDATE SET "
                "last_cursor=excluded.last_cursor,running=excluded.running,failed=excluded.failed");
            for (const QVariant& value :
                 QVariantList{session, generation, turn, id, cursor, cursor, group,
                              kind.isNull() ? QStringLiteral("") : kind, running, failed})
                upsert.addBindValue(value);
            if (!exec(upsert, true))
                return false;
        } else if (known) {
            QSqlQuery update(database_);
            update.prepare("UPDATE activity_items SET last_cursor=? WHERE session_id=? AND "
                           "generation=? AND turn_sequence=? AND item_id=?");
            for (const QVariant& value : QVariantList{cursor, session, generation, turn, id})
                update.addBindValue(value);
            if (!exec(update, true))
                return false;
        }
        if (group > 0) {
            QSqlQuery aggregate(database_);
            aggregate.prepare(
                "INSERT INTO activity_summaries VALUES(?,?,?,?,?,?,?,?,?) ON "
                "CONFLICT(session_id,generation,group_cursor,family) DO UPDATE SET "
                "count=count+excluded.count,running=running+excluded.running,failed=failed+"
                "excluded.failed,last_cursor=MAX(last_cursor,excluded.last_cursor)");
            for (const QVariant& value :
                 QVariantList{session, generation, group, kind, known ? 0 : 1, running - wasRunning,
                              failed - wasFailed, cursor, turn})
                aggregate.addBindValue(value);
            if (!exec(aggregate, true))
                return false;
        }
    } else if (type == "user.message" || type == "turn.steered" || type == "approval.requested" ||
               type.startsWith("provider.") || type == "runtime.error" ||
               type == "turn.completed") {
        head = 0;
    }
    if (type == "turn.completed" || type == "runtime.error") {
        for (const auto& table :
             {QStringLiteral("activity_items"), QStringLiteral("activity_summaries")}) {
            QSqlQuery settle(database_);
            settle.prepare(
                QStringLiteral("UPDATE %1 SET running=0,last_cursor=? WHERE session_id=? AND "
                               "generation=? AND turn_sequence=? AND running>0")
                    .arg(table));
            for (const QVariant& value : QVariantList{cursor, session, generation, turn})
                settle.addBindValue(value);
            if (!exec(settle))
                return false;
        }
    }
    QSqlQuery update(database_);
    update.prepare("INSERT INTO activity_heads VALUES(?,?,?,?,?) ON "
                   "CONFLICT(session_id,generation) DO UPDATE SET "
                   "turn_sequence=excluded.turn_sequence,group_cursor=excluded.group_cursor,"
                   "indexed_cursor=excluded.indexed_cursor");
    for (const QVariant& value : QVariantList{session, generation, turn, head, cursor})
        update.addBindValue(value);
    return exec(update, true);
}

QJsonObject LocalJournal::activitySummaryEvent(const QJsonObject& event) const {
    const auto type = event.value("event_type").toString();
    if (type != "item.started" && type != "item.completed" && type != "command.output_delta" &&
        type != "transcript.large_chunk")
        return event;
    const auto payload = event.value("payload").toObject();
    const QString id = itemId(payload);
    QSqlQuery member(database_);
    member.prepare("SELECT group_cursor FROM activity_items WHERE session_id=? AND generation=? "
                   "AND turn_sequence=? AND item_id=?");
    member.addBindValue(event.value("session_id").toString());
    member.addBindValue(event.value("session_generation").toInteger());
    member.addBindValue(event.value("turn_sequence").toInteger());
    member.addBindValue(id);
    if (!member.exec() || !member.next() || member.value(0).toLongLong() == 0)
        return event;
    const auto group = member.value(0).toLongLong();
    QSqlQuery summary(database_);
    summary.prepare("SELECT family,count,running,failed,last_cursor FROM activity_summaries WHERE "
                    "session_id=? AND generation=? AND group_cursor=? ORDER BY family");
    summary.addBindValue(event.value("session_id").toString());
    summary.addBindValue(event.value("session_generation").toInteger());
    summary.addBindValue(group);
    if (!summary.exec())
        return event;
    QJsonArray families;
    int count = 0, running = 0, failed = 0;
    qint64 watermark = 0;
    while (summary.next()) {
        const int n = summary.value(1).toInt();
        count += n;
        running += summary.value(2).toInt();
        failed += summary.value(3).toInt();
        watermark = qMax(watermark, summary.value(4).toLongLong());
        families.append(QJsonObject{{"family", summary.value(0).toString()}, {"count", n}});
    }
    auto projected = event;
    projected.insert("event_type", "activity.group.summary");
    projected.insert("payload", QJsonObject{{"group_id", QString::number(group)},
                                            {"item_id", "activity-group:" + QString::number(group)},
                                            {"count", count},
                                            {"running_count", running},
                                            {"failed_count", failed},
                                            {"revision", watermark},
                                            {"last_change_cursor", watermark},
                                            {"families", families}});
    return projected;
}

QJsonObject LocalJournal::activityGroupPage(const QString& session, qint64 generation,
                                            const QJsonObject& params) const {
    const QString groupId = params.value("group_id").toString();
    bool numeric = false;
    const auto group = groupId.toLongLong(&numeric);
    const auto after = params.value("after_cursor").toInteger(0);
    const auto requestedSnapshot = params.value("snapshot_cursor").toInteger(-1);
    const auto requestedRevision = params.value("projection_revision").toInteger(-1);
    const int limit = params.value("event_limit").toInt(128);
    if (!numeric || group <= 0 || after < 0 || limit < 1 || limit > 128)
        return failure("invalid_page", "Invalid activity group boundary.");
    struct ReadSnapshot {
        QSqlDatabase db;
        ~ReadSnapshot() { db.rollback(); }
    };
    auto readDatabase = database_;
    if (!readDatabase.transaction())
        return failure("journal_error", database_.lastError().text());
    ReadSnapshot transaction{database_};
    QSqlQuery fence(database_);
    fence.prepare("SELECT s.last_cursor,COALESCE(p.revision,0) FROM sessions s LEFT JOIN "
                  "replay_projection_state p ON p.session_id=s.id AND p.generation=s.generation "
                  "WHERE s.id=? AND s.generation=?");
    fence.addBindValue(session);
    fence.addBindValue(generation);
    if (!fence.exec() || !fence.next())
        return failure("stale_generation", "Unknown session generation.");
    const qint64 head = fence.value(0).toLongLong(), revision = fence.value(1).toLongLong();
    const qint64 snapshot = requestedSnapshot < 0 ? head : requestedSnapshot;
    if (snapshot > head || after > snapshot || (requestedSnapshot >= 0 && requestedRevision < 0))
        return failure("invalid_page", "Activity cursor is out of range.");
    if (requestedRevision >= 0 && requestedRevision != revision)
        return failure("snapshot_changed", "Activity snapshot changed; reload the group.");
    bool groupRunning = false;
    QSqlQuery exists(database_);
    exists.prepare("SELECT SUM(running),COUNT(*) FROM activity_summaries WHERE session_id=? AND "
                   "generation=? AND group_cursor=?");
    exists.addBindValue(session);
    exists.addBindValue(generation);
    exists.addBindValue(group);
    if (!exists.exec() || !exists.next() || exists.value(1).toInt() == 0)
        return failure("unknown_group", "This activity group is not retained.");
    groupRunning = exists.value(0).toInt() > 0;
    QSqlQuery rows(database_);
    rows.prepare("SELECT r.cursor,r.turn_sequence,r.event_type,r.payload,r.sent_at FROM "
                 "replay_events r JOIN activity_items a ON a.session_id=r.session_id AND "
                 "a.generation=r.generation AND a.turn_sequence=r.turn_sequence AND "
                 "a.item_id=r.item_id WHERE r.session_id=? AND r.generation=? AND a.group_cursor=? "
                 "AND r.event_type IN ('item.started','item.completed','command.output_delta') AND "
                 "r.cursor>? AND r.cursor<=? ORDER BY r.cursor LIMIT ?");
    for (const QVariant& value :
         QVariantList{session, generation, group, after, snapshot, limit + 1})
        rows.addBindValue(value);
    if (!rows.exec())
        return failure("journal_error", rows.lastError().text());
    QJsonArray events;
    qint64 next = after;
    qsizetype bytes = 0;
    bool more = false;
    while (rows.next()) {
        QJsonObject event{
            {"session_id", session},
            {"session_generation", generation},
            {"cursor", rows.value(0).toLongLong()},
            {"turn_sequence", rows.value(1).toLongLong()},
            {"event_type", rows.value(2).toString()},
            {"payload", QJsonDocument::fromJson(rows.value(3).toString().toUtf8()).object()},
            {"sent_at", rows.value(4).toString()}};
        const auto size = QJsonDocument(event).toJson(QJsonDocument::Compact).size();
        if (events.size() == limit || bytes + size > 512 * 1024) {
            more = true;
            break;
        }
        bytes += size;
        next = rows.value(0).toLongLong();
        events.append(event);
    }
    if (more && events.isEmpty())
        return failure("event_too_large", "Activity exceeds the bounded page.");
    return {{"ok", true},
            {"group_id", groupId},
            {"session_id", session},
            {"session_generation", generation},
            {"events", events},
            {"snapshot_cursor", snapshot},
            {"projection_revision", revision},
            {"group_running", groupRunning},
            {"next_cursor", more ? next : snapshot},
            {"has_more", more}};
}
} // namespace ketplus::daemon
