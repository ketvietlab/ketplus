#include "storage/JournalStorageReader.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <algorithm>

namespace ketplus::daemon::journal_reader {
namespace {
// A read-only connection owned by the calling thread, removed when it goes out of scope.
class Reader final {
  public:
    explicit Reader(const QString& path)
        : name_(QStringLiteral("ketplus-journal-reader-%1")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))) {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
        database.setDatabaseName(path);
        database.setConnectOptions(
            QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
        if (!database.open())
            error_ = database.lastError().text();
    }
    ~Reader() {
        {
            auto database = QSqlDatabase::database(name_, false);
            database.close();
        }
        QSqlDatabase::removeDatabase(name_);
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    [[nodiscard]] QSqlDatabase database() const { return QSqlDatabase::database(name_, false); }
    [[nodiscard]] QString error() const { return error_; }

  private:
    QString name_;
    QString error_;
};

// Sets owner-only permissions and confirms they took, so a file system that ignores them
// fails the export rather than leaving transcripts readable by others.
bool ownerOnly(const QString& path, const QFileDevice::Permissions permissions) {
    constexpr auto others = QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                            QFileDevice::ExeGroup | QFileDevice::ReadOther |
                            QFileDevice::WriteOther | QFileDevice::ExeOther;
    return QFile::setPermissions(path, permissions) &&
           (QFile::permissions(path) & others) == QFileDevice::Permissions{};
}

bool scalar(QSqlDatabase database, const QString& sql, qint64* value, QString* error) {
    QSqlQuery query(database);
    if (!query.exec(sql) || !query.next()) {
        *error = query.lastError().text();
        return false;
    }
    *value = query.value(0).toLongLong();
    return true;
}

bool isImage(const QString& type) {
    return type == QStringLiteral("imageGeneration") || type == QStringLiteral("imageView");
}
bool isCommand(const QString& type) { return type == QStringLiteral("commandExecution"); }
bool isTool(const QString& type) {
    return type == QStringLiteral("mcpToolCall") || type == QStringLiteral("dynamicToolCall");
}

struct Conversation final {
    QString id;
    qint64 bytes{0};
    qint64 imageBytes{0}, commandBytes{0}, toolBytes{0};
    qint64 images{0}, commands{0}, tools{0};
};

QString note(const Conversation& conversation) {
    const auto plural = [](qint64 count, const QString& one, const QString& many) {
        return QStringLiteral("%1 %2").arg(QLocale(QLocale::English).toString(count),
                                           count == 1 ? one : many);
    };
    const auto largest =
        std::max({conversation.imageBytes, conversation.commandBytes, conversation.toolBytes});
    if (largest <= 0)
        return {};
    if (largest == conversation.imageBytes)
        return plural(conversation.images, QStringLiteral("generated image"),
                      QStringLiteral("generated images"));
    if (largest == conversation.toolBytes)
        return plural(conversation.tools, QStringLiteral("tool result"),
                      QStringLiteral("tool results"));
    return plural(conversation.commands, QStringLiteral("command"), QStringLiteral("commands"));
}
} // namespace

std::optional<QJsonObject> measure(const QString& databasePath, QString* error) {
    Reader reader(databasePath);
    if (!reader.error().isEmpty()) {
        *error = reader.error();
        return std::nullopt;
    }
    auto database = reader.database();
    qint64 pageSize = 0, pageCount = 0, freePages = 0;
    if (!scalar(database, QStringLiteral("PRAGMA page_size"), &pageSize, error) ||
        !scalar(database, QStringLiteral("PRAGMA page_count"), &pageCount, error) ||
        !scalar(database, QStringLiteral("PRAGMA freelist_count"), &freePages, error))
        return std::nullopt;
    const qint64 databaseBytes = pageSize * pageCount;
    const qint64 freeBytes = pageSize * freePages;

    // The activity index is measured in pages where SQLite offers dbstat (it does on macOS);
    // elsewhere its rows are summed, which leaves out index pages.
    qint64 activityBytes = 0;
    bool pages = true;
    {
        QSqlQuery query(database);
        if (query.exec(QStringLiteral(
                "SELECT COALESCE(SUM(d.pgsize),0) FROM dbstat d JOIN sqlite_schema s "
                "ON s.name=d.name WHERE d.aggregate=TRUE AND s.tbl_name LIKE 'activity%'")) &&
            query.next()) {
            activityBytes = query.value(0).toLongLong();
        } else {
            pages = false;
        }
    }
    qint64 usageBytes = 0;
    if (!scalar(database,
                QStringLiteral("SELECT COALESCE(SUM(length(CAST(payload AS BLOB))),0) FROM "
                               "replay_events WHERE event_type LIKE 'usage.%'"),
                &usageBytes, error))
        return std::nullopt;
    activityBytes += usageBytes;

    // One pass over completed items, by conversation and kind. A large item's bytes live in
    // its payload chunks; its replay row holds only the manifest.
    QHash<QString, Conversation> conversations;
    qint64 imageBytes = 0, imageCount = 0;
    {
        QSqlQuery query(database);
        query.setForwardOnly(true);
        if (!query.exec(QStringLiteral(
                "SELECT r.session_id, COALESCE(json_extract(r.payload,'$.item.type'),"
                "json_extract(r.payload,'$.item_type'),''), COUNT(*), "
                "SUM(length(CAST(r.payload AS BLOB)) + COALESCE((SELECT p.total_bytes FROM "
                "large_event_payloads p WHERE p.session_id=r.session_id AND "
                "p.generation=r.generation AND p.transfer_id="
                "json_extract(r.payload,'$.large_event.transfer_id')),0)) "
                "FROM replay_events r WHERE r.event_type='item.completed' "
                "AND json_valid(r.payload) GROUP BY 1,2"))) {
            *error = query.lastError().text();
            return std::nullopt;
        }
        while (query.next()) {
            const auto id = query.value(0).toString();
            const auto type = query.value(1).toString();
            const auto count = query.value(2).toLongLong();
            const auto bytes = query.value(3).toLongLong();
            auto& conversation = conversations[id];
            conversation.id = id;
            if (isImage(type)) {
                conversation.imageBytes += bytes;
                conversation.images += count;
                imageBytes += bytes;
                imageCount += count;
            } else if (isCommand(type)) {
                conversation.commandBytes += bytes;
                conversation.commands += count;
            } else if (isTool(type)) {
                conversation.toolBytes += bytes;
                conversation.tools += count;
            }
        }
        if (query.lastError().isValid()) {
            *error = query.lastError().text();
            return std::nullopt;
        }
    }
    {
        QSqlQuery query(database);
        query.setForwardOnly(true);
        if (!query.exec(QStringLiteral(
                "SELECT session_id, SUM(bytes) FROM ("
                "SELECT session_id, length(CAST(payload AS BLOB)) AS bytes FROM replay_events "
                "UNION ALL SELECT session_id, total_bytes FROM large_event_payloads "
                "UNION ALL SELECT session_id, length(CAST(payload AS BLOB)) FROM events) "
                "GROUP BY session_id"))) {
            *error = query.lastError().text();
            return std::nullopt;
        }
        while (query.next()) {
            auto& conversation = conversations[query.value(0).toString()];
            conversation.id = query.value(0).toString();
            conversation.bytes = query.value(1).toLongLong();
        }
    }
    QList<Conversation> ranked = conversations.values();
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        return left.bytes != right.bytes ? left.bytes > right.bytes : left.id < right.id;
    });
    QJsonArray largest;
    QSqlQuery details(database);
    details.prepare(QStringLiteral(
        "SELECT s.title, s.adapter, (SELECT n.number FROM conversation_numbers n WHERE "
        "n.session_id=s.id AND n.generation=s.generation AND n.released_at IS NULL LIMIT 1) "
        "FROM sessions s WHERE s.id=?"));
    for (const auto& conversation : std::as_const(ranked)) {
        if (largest.size() == 5 || conversation.bytes <= 0)
            break;
        details.bindValue(0, conversation.id);
        // Numbering may be missing on an older journal; the row still lists the conversation.
        if (!details.exec() || !details.next())
            continue; // deleted, only its leftovers remain
        QJsonObject row{{"session_id", conversation.id},
                        {"title", details.value(0).toString()},
                        {"provider", details.value(1).toString()},
                        {"bytes", conversation.bytes},
                        {"note", note(conversation)}};
        if (!details.value(2).isNull())
            row.insert("number", details.value(2).toString());
        largest.append(row);
        details.finish();
    }

    QString oldest;
    {
        QSqlQuery query(database);
        if (query.exec(QStringLiteral("SELECT MIN(sent_at) FROM replay_events")) && query.next())
            oldest = query.value(0).toString();
    }
    const qint64 kept = databaseBytes - freeBytes;
    imageBytes = qBound<qint64>(0, imageBytes, kept);
    activityBytes = qBound<qint64>(0, activityBytes, kept - imageBytes);
    QJsonObject result{{"database_page_bytes", databaseBytes},
                       {"parts", QJsonObject{{"history_bytes", kept - imageBytes - activityBytes},
                                             {"image_bytes", imageBytes},
                                             {"image_count", imageCount},
                                             {"activity_bytes", activityBytes},
                                             {"free_bytes", freeBytes},
                                             {"measured_in", pages ? QStringLiteral("pages")
                                                                   : QStringLiteral("rows")}}},
                       {"largest", largest},
                       {"measured_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                       {"directory", QFileInfo(databasePath).absolutePath()}};
    if (!oldest.isEmpty())
        result.insert("oldest_event_at", oldest);
    return result;
}

namespace {
QString slug(const QString& title) {
    QString result;
    for (const QChar character : title.left(64)) {
        if (character.isLetterOrNumber())
            result.append(character.toLower());
        else if (!result.isEmpty() && !result.endsWith(QLatin1Char('-')))
            result.append(QLatin1Char('-'));
    }
    while (result.endsWith(QLatin1Char('-')))
        result.chop(1);
    return result.isEmpty() ? QStringLiteral("conversation") : result;
}

// The replay payload with a large item put back together, so the file stands on its own.
std::optional<QJsonObject> hydrated(QSqlQuery& chunks, const QString& session, qint64 generation,
                                    const QJsonObject& payload, QString* error) {
    const auto transfer = payload.value("large_event").toObject().value("transfer_id").toString();
    if (transfer.isEmpty())
        return payload;
    chunks.bindValue(0, session);
    chunks.bindValue(1, generation);
    chunks.bindValue(2, transfer);
    if (!chunks.exec()) {
        *error = chunks.lastError().text();
        return std::nullopt;
    }
    QByteArray bytes;
    while (chunks.next())
        bytes.append(chunks.value(0).toByteArray());
    chunks.finish();
    const auto document = QJsonDocument::fromJson(bytes);
    // A payload the pass already removed stays as its manifest.
    return document.isObject() ? document.object() : payload;
}
} // namespace

std::optional<QJsonObject> exportConversations(const QString& databasePath,
                                               const QString& directory,
                                               const std::function<bool(int, int)>& progress,
                                               QString* error) {
    const QFileInfo parent(directory);
    if (!parent.isAbsolute() || !parent.isDir() || !parent.isWritable()) {
        *error = QStringLiteral("Choose a folder you can write to.");
        return std::nullopt;
    }
    Reader reader(databasePath);
    if (!reader.error().isEmpty()) {
        *error = reader.error();
        return std::nullopt;
    }
    auto database = reader.database();
    // One read transaction for the whole export: every conversation listed below is read from
    // the same snapshot, even if it is deleted or expires while the export runs.
    if (!database.transaction()) {
        *error = database.lastError().text();
        return std::nullopt;
    }
    struct Row final {
        QString id, title, adapter, worktree, archivedAt, number;
        qint64 generation{0};
    };
    QList<Row> rows;
    {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral(
                "SELECT s.id, s.generation, s.title, s.adapter, s.worktree_path, "
                "COALESCE(s.archived_at,''), COALESCE((SELECT n.number FROM conversation_numbers "
                "n WHERE n.session_id=s.id AND n.generation=s.generation AND n.released_at IS "
                "NULL LIMIT 1),'') FROM sessions s ORDER BY s.id"))) {
            *error = query.lastError().text();
            return std::nullopt;
        }
        while (query.next())
            rows.append({query.value(0).toString(), query.value(2).toString(),
                         query.value(3).toString(), query.value(4).toString(),
                         query.value(5).toString(), query.value(6).toString(),
                         query.value(1).toLongLong()});
    }
    const QString name = QDir(directory).filePath(
        QStringLiteral("KetPlus conversations %1")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm.ss"))));
    QString folder = name;
    for (int suffix = 2; QFileInfo::exists(folder); ++suffix)
        folder = name + QStringLiteral(" %1").arg(suffix);
    if (!QDir().mkpath(folder)) {
        *error = QStringLiteral("Could not create %1.").arg(folder);
        return std::nullopt;
    }
    // Transcripts may hold secrets the agents saw: owner-only, like the journal.
    if (!ownerOnly(folder,
                   QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        QDir().rmdir(folder);
        *error = QStringLiteral("Could not make %1 private to you.").arg(folder);
        return std::nullopt;
    }
    QSqlQuery events(database);
    events.setForwardOnly(true);
    events.prepare(QStringLiteral(
        "SELECT cursor, event_type, item_id, payload, sent_at, turn_sequence FROM replay_events "
        "WHERE session_id=? AND generation=? ORDER BY cursor"));
    QSqlQuery chunks(database);
    chunks.setForwardOnly(true);
    chunks.prepare(QStringLiteral(
        "SELECT payload FROM large_event_payload_chunks WHERE session_id=? AND generation=? AND "
        "transfer_id=? ORDER BY chunk_offset"));
    int exported = 0;
    for (const auto& row : std::as_const(rows)) {
        if (!progress(exported, int(rows.size()))) {
            *error = QStringLiteral("Export stopped.");
            return std::nullopt;
        }
        const QString base = (row.number.isEmpty() ? row.id.left(8) : row.number) +
                             QLatin1Char('-') + slug(row.title);
        QString path = QDir(folder).filePath(base + QStringLiteral(".json"));
        for (int suffix = 2; QFileInfo::exists(path); ++suffix)
            path = QDir(folder).filePath(base + QStringLiteral("-%1.json").arg(suffix));
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            *error = file.errorString();
            return std::nullopt;
        }
        QJsonObject conversation{{"session_id", row.id},
                                 {"generation", row.generation},
                                 {"title", row.title},
                                 {"provider", row.adapter},
                                 {"worktree_path", row.worktree}};
        if (!row.number.isEmpty())
            conversation.insert("number", row.number);
        if (!row.archivedAt.isEmpty())
            conversation.insert("archived_at", row.archivedAt);
        // Streamed one event at a time; a large item is at most 32 MiB.
        file.write(R"({"format":"ketplus.conversation-export/1","conversation":)");
        file.write(QJsonDocument(conversation).toJson(QJsonDocument::Compact));
        file.write(R"(,"events":[)");
        events.bindValue(0, row.id);
        events.bindValue(1, row.generation);
        if (!events.exec()) {
            *error = events.lastError().text();
            return std::nullopt;
        }
        bool first = true;
        while (events.next()) {
            const auto payload =
                hydrated(chunks, row.id, row.generation,
                         QJsonDocument::fromJson(events.value(3).toByteArray()).object(), error);
            if (!payload)
                return std::nullopt;
            const QJsonObject event{
                {"cursor", events.value(0).toLongLong()}, {"type", events.value(1).toString()},
                {"item_id", events.value(2).toString()},  {"sent_at", events.value(4).toString()},
                {"turn", events.value(5).toLongLong()},   {"payload", *payload}};
            if (!first)
                file.write(",");
            first = false;
            file.write(QJsonDocument(event).toJson(QJsonDocument::Compact));
        }
        events.finish();
        file.write("]}\n");
        if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            file.cancelWriting();
            *error = QStringLiteral("Could not make %1 private to you.").arg(path);
            return std::nullopt;
        }
        if (!file.commit()) {
            *error = file.errorString();
            return std::nullopt;
        }
        if (!ownerOnly(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            QFile::remove(path);
            *error = QStringLiteral("Could not make %1 private to you.").arg(path);
            return std::nullopt;
        }
        ++exported;
    }
    events.finish();
    chunks.finish();
    database.rollback(); // read-only: ends the snapshot
    progress(exported, int(rows.size()));
    return QJsonObject{{"directory", folder}, {"exported", exported}};
}
} // namespace ketplus::daemon::journal_reader
