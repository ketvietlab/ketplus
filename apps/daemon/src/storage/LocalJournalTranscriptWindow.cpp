#include "storage/LocalJournal.h"
#include "storage/TranscriptItemSnapshot.h"
#include "storage/TranscriptWindowQueries.h"

#include <QJsonDocument>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {
namespace {

constexpr qsizetype maximumWindowBytes = 512 * 1024;
constexpr int maximumWindowEvents = 128;

// One short SQLite read transaction per page, never held across IPC requests.
class ReadTransaction final {
  public:
    explicit ReadTransaction(const QSqlDatabase& database) : database_(database) {
        opened_ = database_.transaction();
    }
    ~ReadTransaction() {
        if (opened_) {
            database_.rollback();
        }
    }
    [[nodiscard]] bool opened() const { return opened_; }

  private:
    QSqlDatabase database_;
    bool opened_{false};
};

} // namespace

TranscriptWindow LocalJournal::transcriptWindow(const QString& sessionId, const qint64 generation,
                                                const qint64 beforeCursor,
                                                const qint64 snapshotCursor,
                                                const qint64 projectionRevision,
                                                const int turnLimit, const int eventLimit) const {
    return transcriptWindowImpl(sessionId, generation, beforeCursor, snapshotCursor,
                                projectionRevision, turnLimit, eventLimit, false);
}

TranscriptWindow
LocalJournal::restoreTranscriptWindow(const QString& sessionId, const qint64 generation,
                                      const qint64 beforeCursor, const qint64 snapshotCursor,
                                      const int turnLimit, const int eventLimit) const {
    return transcriptWindowImpl(sessionId, generation, beforeCursor, snapshotCursor, -1, turnLimit,
                                eventLimit, true);
}

TranscriptWindow
LocalJournal::transcriptWindowAfter(const QString& sessionId, const qint64 generation,
                                    const qint64 afterCursor, const qint64 snapshotCursor,
                                    const qint64 projectionRevision, const int turnLimit,
                                    const int eventLimit) const {
    if (afterCursor < 1) {
        TranscriptWindow failed;
        failed.errorCode = QStringLiteral("invalid_page");
        failed.errorMessage = QStringLiteral("Invalid window boundary.");
        return failed;
    }
    return transcriptWindowImpl(sessionId, generation, 0, snapshotCursor, projectionRevision,
                                turnLimit, eventLimit, false, afterCursor);
}

TranscriptWindow LocalJournal::transcriptWindowImpl(
    const QString& sessionId, const qint64 generation, const qint64 beforeCursor,
    const qint64 snapshotCursor, const qint64 projectionRevision, const int turnLimit,
    const int eventLimit, const bool bindCurrentProjection, const qint64 afterCursor) const {
    setError({});
    TranscriptWindow page;
    const auto fail = [&](const QString& code, const QString& message) {
        if (code == QStringLiteral("journal_error")) {
            setError(message);
        }
        TranscriptWindow failed;
        failed.errorCode = code;
        failed.errorMessage = message;
        return failed;
    };
    const bool forward = afterCursor > 0;
    const bool continuing = beforeCursor > 0 || forward;
    if (beforeCursor < 0 || afterCursor < 0 ||
        (forward && (beforeCursor > 0 || bindCurrentProjection)) || turnLimit < 1 ||
        turnLimit > maximumTranscriptWindowTurns || eventLimit < 1 ||
        eventLimit > maximumWindowEvents ||
        (bindCurrentProjection ? snapshotCursor < 0 || projectionRevision != -1
                               : (continuing ? snapshotCursor < 0 || projectionRevision < 0
                                             : snapshotCursor != -1 || projectionRevision != -1))) {
        return fail(QStringLiteral("invalid_page"), QStringLiteral("Invalid window boundary."));
    }
    ReadTransaction transaction(database_);
    if (!transaction.opened()) {
        return fail(QStringLiteral("journal_error"), database_.lastError().text());
    }
    QSqlQuery snapshot(database_);
    snapshot.prepare(QStringLiteral(
        "SELECT s.last_cursor, COALESCE(p.revision, 0) FROM sessions s LEFT JOIN "
        "replay_projection_state p ON p.session_id = s.id AND p.generation = s.generation "
        "WHERE s.id = ? AND s.generation = ?"));
    snapshot.addBindValue(sessionId);
    snapshot.addBindValue(generation);
    if (!snapshot.exec()) {
        return fail(QStringLiteral("journal_error"), snapshot.lastError().text());
    }
    if (!snapshot.next()) {
        return fail(QStringLiteral("stale_generation"),
                    QStringLiteral("Unknown session generation."));
    }
    const qint64 currentCursor = snapshot.value(0).toLongLong();
    const qint64 currentRevision = snapshot.value(1).toLongLong();
    page.snapshotCursor = bindCurrentProjection || continuing ? snapshotCursor : currentCursor;
    page.projectionRevision = currentRevision;
    // A restore names the head the relay last heard, which trails a running turn by up to a
    // heartbeat. Events are appended, so the window is served as of that cursor and the phone
    // reads newer ones live. A compaction since then only drops deltas whose completed item,
    // carrying the full text, lies after the cursor.
    if ((continuing || bindCurrentProjection) &&
        (snapshotCursor > currentCursor || beforeCursor > snapshotCursor + 1 ||
         afterCursor > snapshotCursor)) {
        return fail(QStringLiteral("invalid_page"),
                    QStringLiteral("Window cursor is out of range."));
    }
    if (!bindCurrentProjection && continuing && projectionRevision != currentRevision) {
        return fail(QStringLiteral("snapshot_changed"),
                    QStringLiteral(
                        "Transcript compaction changed this snapshot; reopen the latest window."));
    }
    const qint64 upper = beforeCursor > 0 ? beforeCursor : page.snapshotCursor + 1;
    qsizetype bytes = 2;
    QList<QJsonObject> reverseEvents;
    const auto readEvent = [&](const QSqlQuery& rows) {
        const auto payload = QJsonDocument::fromJson(rows.value(3).toString().toUtf8()).object();
        return QJsonObject{{QStringLiteral("session_id"), sessionId},
                           {QStringLiteral("session_generation"), generation},
                           {QStringLiteral("cursor"), rows.value(0).toLongLong()},
                           {QStringLiteral("turn_sequence"), rows.value(1).toLongLong()},
                           {QStringLiteral("event_type"), rows.value(2).toString()},
                           {QStringLiteral("payload"), payload},
                           {QStringLiteral("sent_at"), rows.value(4).toString()}};
    };
    // Reserve identity metadata as well, including maliciously large item IDs.
    const auto encodedBytes = [](const QJsonObject& event, const QString& itemId) {
        return QJsonDocument(event).toJson(QJsonDocument::Compact).size() +
               QJsonDocument(QJsonObject{{"item_id", itemId}})
                   .toJson(QJsonDocument::Compact)
                   .size() +
               96;
    };
    if (forward) {
        // Seek at most turnLimit turn boundaries forward, then read the oldest bounded prefix.
        QSqlQuery first(database_);
        first.prepare(QString::fromLatin1(transcript_window_queries::firstTurnAfter));
        first.addBindValue(sessionId);
        first.addBindValue(generation);
        first.addBindValue(afterCursor);
        first.addBindValue(page.snapshotCursor);
        if (!first.exec()) {
            return fail(QStringLiteral("journal_error"), first.lastError().text());
        }
        if (!first.next()) {
            page.afterCursor = afterCursor;
            return page;
        }
        qint64 turnSequence = first.value(0).toLongLong();
        qint64 limit = page.snapshotCursor + 1;
        for (int index = 0; index < turnLimit; ++index) {
            QSqlQuery next(database_);
            next.prepare(QString::fromLatin1(transcript_window_queries::nextTurn));
            next.addBindValue(sessionId);
            next.addBindValue(generation);
            next.addBindValue(turnSequence);
            next.addBindValue(page.snapshotCursor);
            if (!next.exec()) {
                return fail(QStringLiteral("journal_error"), next.lastError().text());
            }
            if (!next.next()) {
                break;
            }
            turnSequence = next.value(0).toLongLong();
            if (index + 1 == turnLimit) {
                QSqlQuery start(database_);
                start.prepare(QString::fromLatin1(transcript_window_queries::turnStart));
                start.addBindValue(sessionId);
                start.addBindValue(generation);
                start.addBindValue(turnSequence);
                if (!start.exec() || !start.next()) {
                    return fail(QStringLiteral("journal_error"), start.lastError().text());
                }
                limit = start.value(0).toLongLong();
            }
        }
        QSqlQuery rows(database_);
        rows.prepare(QString::fromLatin1(transcript_window_queries::windowAfter));
        rows.addBindValue(sessionId);
        rows.addBindValue(generation);
        rows.addBindValue(afterCursor);
        rows.addBindValue(limit);
        rows.addBindValue(eventLimit);
        if (!rows.exec()) {
            return fail(QStringLiteral("journal_error"), rows.lastError().text());
        }
        QList<QJsonObject> ascending;
        while (rows.next()) {
            const QJsonObject event = readEvent(rows);
            const qsizetype eventBytes = encodedBytes(event, rows.value(5).toString());
            if (bytes + eventBytes > maximumWindowBytes) {
                if (ascending.isEmpty()) {
                    return fail(QStringLiteral("event_too_large"),
                                QStringLiteral("An event exceeds the bounded transcript window."));
                }
                break;
            }
            bytes += eventBytes;
            ascending.append(event);
        }
        for (auto it = ascending.crbegin(); it != ascending.crend(); ++it) {
            reverseEvents.append(*it);
        }
    }

    // Seek at most turnLimit turn boundaries. DISTINCT/GROUP BY over the whole
    // conversation would visit every delta of a giant turn before returning.
    qint64 earliestTurn = -1;
    for (int index = 0; !forward && index < turnLimit; ++index) {
        QSqlQuery turn(database_);
        turn.prepare(QString::fromLatin1(earliestTurn >= 0
                                             ? transcript_window_queries::previousTurn
                                             : transcript_window_queries::latestTurn));
        turn.addBindValue(sessionId);
        turn.addBindValue(generation);
        if (earliestTurn >= 0) {
            turn.addBindValue(earliestTurn);
        } else {
            turn.addBindValue(upper);
        }
        if (!turn.exec()) {
            return fail(QStringLiteral("journal_error"), turn.lastError().text());
        }
        if (!turn.next()) {
            break;
        }
        earliestTurn = turn.value(0).toLongLong();
    }
    if (!forward && earliestTurn < 0) {
        return page;
    }
    if (!forward) {
        QSqlQuery start(database_);
        start.prepare(QString::fromLatin1(transcript_window_queries::turnStart));
        start.addBindValue(sessionId);
        start.addBindValue(generation);
        start.addBindValue(earliestTurn);
        if (!start.exec() || !start.next()) {
            return fail(QStringLiteral("journal_error"), start.lastError().text());
        }
        const qint64 lower = start.value(0).toLongLong();
        QSqlQuery rows(database_);
        rows.prepare(QString::fromLatin1(transcript_window_queries::window));
        rows.addBindValue(sessionId);
        rows.addBindValue(generation);
        rows.addBindValue(lower);
        rows.addBindValue(upper);
        rows.addBindValue(eventLimit);
        if (!rows.exec()) {
            return fail(QStringLiteral("journal_error"), rows.lastError().text());
        }
        while (rows.next()) {
            const QJsonObject event = readEvent(rows);
            const qsizetype eventBytes = encodedBytes(event, rows.value(5).toString());
            if (bytes + eventBytes > maximumWindowBytes) {
                if (reverseEvents.isEmpty()) {
                    return fail(QStringLiteral("event_too_large"),
                                QStringLiteral("An event exceeds the bounded transcript window."));
                }
                break;
            }
            bytes += eventBytes;
            reverseEvents.append(event);
        }
    }
    QSet<QPair<qint64, QString>> inspectedItems;
    for (auto it = reverseEvents.crbegin(); it != reverseEvents.crend(); ++it) {
        page.events.append(*it);
        const QJsonObject payload = it->value(QStringLiteral("payload")).toObject();
        QString itemId = payload.value(QStringLiteral("item_id")).toString();
        if (itemId.isEmpty()) {
            itemId = payload.value(QStringLiteral("item"))
                         .toObject()
                         .value(QStringLiteral("id"))
                         .toString();
        }
        const qint64 turnSequence = it->value(QStringLiteral("turn_sequence")).toInteger();
        const QPair<qint64, QString> itemKey{turnSequence, itemId};
        if (itemId.isEmpty() || inspectedItems.contains(itemKey)) {
            continue;
        }
        inspectedItems.insert(itemKey);
        const QJsonObject largeEvent = payload.value(QStringLiteral("large_event")).toObject();
        if (!largeEvent.isEmpty()) {
            page.partialItems.append(QJsonObject{
                {QStringLiteral("item_id"), itemId},
                {QStringLiteral("turn_sequence"), turnSequence},
                {QStringLiteral("first_cursor"), it->value(QStringLiteral("cursor"))},
                {QStringLiteral("transfer_id"), largeEvent.value(QStringLiteral("transfer_id"))}});
            continue;
        }
        const auto type = it->value(QStringLiteral("event_type")).toString();
        const auto item = payload.value(QStringLiteral("item")).toObject();
        const auto text = item.isEmpty() ? payload.value(QStringLiteral("text"))
                                         : item.value(QStringLiteral("text"));
        const bool missingAssistantSnapshot =
            (type == QStringLiteral("item.started") || type == QStringLiteral("item.completed")) &&
            isAssistantItem(payload) &&
            (!hasAuthoritativeAssistantText(payload) ||
             (type == QStringLiteral("item.started") && text.toString().isEmpty()));
        if (type != QStringLiteral("assistant.delta") && !missingAssistantSnapshot) {
            continue;
        }
        QSqlQuery context(database_);
        context.prepare(QString::fromLatin1(transcript_window_queries::previousItemContext));
        context.addBindValue(sessionId);
        context.addBindValue(generation);
        context.addBindValue(itemId);
        context.addBindValue(turnSequence);
        context.addBindValue(it->value(QStringLiteral("cursor")).toInteger());
        if (!context.exec()) {
            return fail(QStringLiteral("journal_error"), context.lastError().text());
        }
        if (context.next()) {
            page.partialItems.append(
                QJsonObject{{QStringLiteral("item_id"), itemId},
                            {QStringLiteral("turn_sequence"), turnSequence},
                            {QStringLiteral("first_cursor"), it->value(QStringLiteral("cursor"))}});
        }
    }
    if (page.events.isEmpty()) {
        if (forward) {
            page.afterCursor = afterCursor;
        }
        return page;
    }
    page.afterCursor = page.events.last().toObject().value(QStringLiteral("cursor")).toInteger();
    {
        QSqlQuery newer(database_);
        newer.prepare(QString::fromLatin1(transcript_window_queries::hasEventAfter));
        newer.addBindValue(sessionId);
        newer.addBindValue(generation);
        newer.addBindValue(page.afterCursor);
        newer.addBindValue(page.snapshotCursor);
        if (!newer.exec()) {
            return fail(QStringLiteral("journal_error"), newer.lastError().text());
        }
        page.hasMoreAfter = newer.next();
    }
    const QJsonObject first = page.events.first().toObject();
    page.beforeCursor = first.value(QStringLiteral("cursor")).toInteger();
    QSqlQuery previous(database_);
    previous.prepare(
        QStringLiteral("SELECT turn_sequence FROM replay_events WHERE session_id = ? "
                       "AND generation = ? AND cursor < ? ORDER BY cursor DESC LIMIT 1"));
    previous.addBindValue(sessionId);
    previous.addBindValue(generation);
    previous.addBindValue(page.beforeCursor);
    if (!previous.exec()) {
        return fail(QStringLiteral("journal_error"), previous.lastError().text());
    }
    page.hasMoreBefore = previous.next();
    page.partialFirstTurn =
        page.hasMoreBefore &&
        previous.value(0).toLongLong() == first.value(QStringLiteral("turn_sequence")).toInteger();
    return page;
}

} // namespace ketplus::daemon
