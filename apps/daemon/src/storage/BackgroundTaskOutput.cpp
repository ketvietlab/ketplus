#include "storage/BackgroundTaskOutput.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <limits>
#include <utility>

namespace ketplus::daemon {
namespace {
constexpr int chunkBytes = BackgroundTaskOutput::maximumPageBytes;

QString encodeCursor(const QString& taskId, qint64 position) {
    return QString::fromLatin1(
        QJsonDocument(QJsonObject{{"task", taskId}, {"position", QString::number(position)}})
            .toJson(QJsonDocument::Compact)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool decodeCursor(const QString& taskId, const QString& cursor, qint64* position) {
    if (cursor.isEmpty()) {
        *position = 0;
        return true;
    }
    if (cursor.size() > 2048)
        return false;
    const auto decoded = QByteArray::fromBase64Encoding(
        cursor.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return false;
    const auto object = QJsonDocument::fromJson(decoded.decoded).object();
    bool ok = false;
    *position = object.value("position").toString().toLongLong(&ok);
    return ok && *position >= 0 && object.value("task").toString() == taskId;
}
} // namespace

BackgroundTaskOutput::BackgroundTaskOutput(QSqlDatabase database)
    : database_(std::move(database)) {}

bool BackgroundTaskOutput::initialize(QString* error) {
    QSqlQuery query(database_);
    for (const auto& statement :
         {"CREATE TABLE IF NOT EXISTS job_output_heads (task_id TEXT PRIMARY KEY, "
          "total_bytes INTEGER NOT NULL CHECK(total_bytes>=0))",
          "CREATE TABLE IF NOT EXISTS job_output_chunks (task_id TEXT NOT NULL, "
          "position INTEGER NOT NULL CHECK(position>=0), bytes BLOB NOT NULL, "
          "PRIMARY KEY(task_id,position))",
          "CREATE TABLE IF NOT EXISTS job_output_producers(task_id TEXT PRIMARY KEY,revision "
          "INTEGER NOT NULL)"}) {
        if (!query.exec(QLatin1String(statement))) {
            if (error)
                *error = query.lastError().text();
            return false;
        }
    }
    return true;
}

bool BackgroundTaskOutput::append(const QString& taskId, const QByteArray& bytes, QString* error,
                                  qint64 producerRevision) {
    if (error)
        error->clear();
    if (taskId.isEmpty()) {
        if (error)
            *error = QStringLiteral("Missing task identity.");
        return false;
    }
    if (bytes.isEmpty())
        return true;
    if (!database_.transaction()) {
        if (error)
            *error = database_.lastError().text();
        return false;
    }
    const auto fail = [&](const QString& message) {
        database_.rollback();
        if (error)
            *error = message;
        return false;
    };
    QSqlQuery query(database_);
    if (producerRevision > 0) {
        query.prepare("SELECT revision FROM job_output_producers WHERE task_id=?");
        query.addBindValue(taskId);
        if (!query.exec())
            return fail(query.lastError().text());
        if (query.next() && query.value(0).toLongLong() >= producerRevision) {
            query.finish();
            database_.rollback();
            return true;
        }
        query.finish();
    }
    query.prepare("SELECT total_bytes FROM job_output_heads WHERE task_id=?");
    query.addBindValue(taskId);
    if (!query.exec())
        return fail(query.lastError().text());
    const qint64 previous = query.next() ? query.value(0).toLongLong() : 0;
    query.finish();
    if (previous > std::numeric_limits<qint64>::max() - bytes.size())
        return fail(QStringLiteral("Output byte counter exhausted."));
    const qint64 total = previous + bytes.size();
    const qint64 floor = qMax(qint64(0), total - maximumBytes);

    // Coalesce the previous short tail: one-byte writes cannot create millions of rows.
    qint64 position = qMax(previous, floor);
    QByteArray pending = bytes.right(qMin(qint64(bytes.size()), maximumBytes));
    if (previous >= floor) {
        query.prepare("SELECT position,bytes FROM job_output_chunks WHERE task_id=? "
                      "ORDER BY position DESC LIMIT 1");
        query.addBindValue(taskId);
        if (!query.exec())
            return fail(query.lastError().text());
        if (query.next()) {
            const qint64 tailPosition = query.value(0).toLongLong();
            const QByteArray tail = query.value(1).toByteArray();
            if (tail.size() < chunkBytes && tailPosition + tail.size() == previous) {
                position = tailPosition;
                pending.prepend(tail);
            }
        }
        query.finish();
    }
    query.prepare("DELETE FROM job_output_chunks WHERE task_id=? AND "
                  "(position>=? OR position+length(bytes)<=?)");
    query.addBindValue(taskId);
    query.addBindValue(position);
    query.addBindValue(floor);
    if (!query.exec())
        return fail(query.lastError().text());

    for (qsizetype offset = 0; offset < pending.size(); offset += chunkBytes) {
        query.prepare("INSERT INTO job_output_chunks(task_id,position,bytes) VALUES(?,?,?)");
        query.addBindValue(taskId);
        query.addBindValue(position + offset);
        query.addBindValue(pending.mid(offset, chunkBytes));
        if (!query.exec())
            return fail(query.lastError().text());
    }
    // Trim the first retained chunk too, so stored payload is bounded to exactly the tail.
    query.prepare("UPDATE job_output_chunks SET bytes=substr(bytes,?-position+1),position=? "
                  "WHERE task_id=? AND position<? AND position+length(bytes)>?");
    query.addBindValue(floor);
    query.addBindValue(floor);
    query.addBindValue(taskId);
    query.addBindValue(floor);
    query.addBindValue(floor);
    if (!query.exec())
        return fail(query.lastError().text());
    query.prepare("INSERT INTO job_output_heads(task_id,total_bytes) VALUES(?,?) "
                  "ON CONFLICT(task_id) DO UPDATE SET total_bytes=excluded.total_bytes");
    query.addBindValue(taskId);
    query.addBindValue(total);
    if (!query.exec())
        return fail(query.lastError().text());
    if (producerRevision > 0) {
        query.prepare("INSERT INTO job_output_producers VALUES(?,?) ON CONFLICT(task_id) "
                      "DO UPDATE SET revision=excluded.revision");
        query.addBindValue(taskId);
        query.addBindValue(producerRevision);
        if (!query.exec())
            return fail(query.lastError().text());
    }
    if (!database_.commit())
        return fail(database_.lastError().text());
    return true;
}

BackgroundOutputPage BackgroundTaskOutput::read(const QString& taskId, const QString& cursor,
                                                int limit, bool textBoundary,
                                                bool flushTrailing) const {
    BackgroundOutputPage page;
    qint64 requested = 0;
    if (taskId.isEmpty() || limit <= 0 || !decodeCursor(taskId, cursor, &requested)) {
        page.error = QStringLiteral("Invalid output cursor or page size.");
        return page;
    }
    limit = qMin(limit, maximumPageBytes);
    QSqlQuery query(database_);
    // A single read statement sees matching head and chunks, even with a second writer.
    query.prepare("SELECT h.total_bytes,c.position,c.bytes FROM job_output_heads h "
                  "LEFT JOIN job_output_chunks c ON c.task_id=h.task_id AND "
                  "c.position+length(c.bytes)>max(?,h.total_bytes-?) "
                  "WHERE h.task_id=? ORDER BY c.position LIMIT 3");
    query.addBindValue(requested);
    query.addBindValue(maximumBytes);
    query.addBindValue(taskId);
    if (!query.exec()) {
        page.error = query.lastError().text();
        return page;
    }
    bool first = true;
    qint64 position = requested;
    while (query.next()) {
        if (first) {
            first = false;
            page.totalBytes = query.value(0).toLongLong();
            if (requested > page.totalBytes) {
                page.error = QStringLiteral("Output cursor is beyond the byte stream.");
                return page;
            }
            const qint64 floor = qMax(qint64(0), page.totalBytes - maximumBytes);
            page.truncated = requested < floor;
            position = qMax(requested, floor);
        }
        if (query.value(1).isNull())
            continue;
        const qint64 chunkPosition = query.value(1).toLongLong();
        const QByteArray chunk = query.value(2).toByteArray();
        const QByteArray fragment = chunk.mid(position - chunkPosition, limit - page.bytes.size());
        page.bytes.append(fragment);
        position += fragment.size();
        if (page.bytes.size() >= limit)
            break;
    }
    if (first && requested != 0) {
        page.error = QStringLiteral("Output cursor is beyond the byte stream.");
        return page;
    }
    if (textBoundary && !page.bytes.isEmpty() && (!flushTrailing || position < page.totalBytes)) {
        // Keep a valid partial UTF-8 sequence for the next byte page. Invalid
        // binary data still decodes with replacement; byte cursors remain exact.
        qsizetype start = page.bytes.size() - 1;
        while (start > 0 && (static_cast<uchar>(page.bytes[start]) & 0xc0) == 0x80)
            --start;
        const auto first = static_cast<uchar>(page.bytes[start]);
        const int expected = first >= 0xc2 && first <= 0xdf   ? 2
                             : first >= 0xe0 && first <= 0xef ? 3
                             : first >= 0xf0 && first <= 0xf4 ? 4
                                                              : 0;
        const auto trailing = page.bytes.size() - start;
        if (expected && trailing < expected) {
            position -= trailing;
            page.bytes.truncate(start);
        }
    }
    page.nextCursor = encodeCursor(taskId, position);
    return page;
}

} // namespace ketplus::daemon
