#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QSet>
#include <QString>

namespace ketplus::daemon {

inline constexpr int maximumHeartbeatSessions = 100;

// The relay answers 413 to a request body above 1 MiB. An event batch stops well short of it,
// leaving room for the request envelope; a single event is capped far below it by the journal.
inline constexpr qsizetype maximumEventUploadBytes = 768 * 1024;
inline constexpr int maximumEventUploadCount = 100;

// Whether one more serialized event of `eventBytes` joins a batch already holding `count` events
// of `bytes`. The first event always does, so an upload can never be empty.
inline bool eventJoinsUpload(const qsizetype bytes, const int count, const qsizetype eventBytes,
                             const int countLimit) {
    return count == 0 || (count < countLimit && bytes + 1 + eventBytes <= maximumEventUploadBytes);
}

// A session whose upload failed waits before its events are sent again: one second, doubling
// with each consecutive failure. Every journaled event of any session starts an upload, so a
// session the relay kept refusing was sent again several times a second. A refusal (a 4xx
// answer) repeats until the session changes on the relay, and waits up to five minutes; a relay
// that cannot be reached or fails waits up to a minute, and a heartbeat that gets through ends
// that wait.
inline constexpr qint64 eventUploadRefusedBackoffCapMilliseconds = 5 * 60 * 1000;
inline constexpr qint64 eventUploadUnavailableBackoffCapMilliseconds = 60 * 1000;

inline qint64 eventUploadBackoffMilliseconds(const int failures, const bool refused) {
    const qint64 cap = refused ? eventUploadRefusedBackoffCapMilliseconds
                               : eventUploadUnavailableBackoffCapMilliseconds;
    return qMin(cap, qint64(1000) << qBound(0, failures - 1, 20));
}

/// Whether the relay itself refused an upload, as opposed to timing out, rate limiting,
/// failing or not being reached at all.
inline bool eventUploadRefused(const int status) {
    return status >= 400 && status < 500 && status != 408 && status != 429;
}

inline QList<QJsonArray> heartbeatSessionBatches(const QJsonArray& sessions) {
    QList<QJsonArray> batches;
    if (sessions.isEmpty()) {
        batches.append(QJsonArray{});
        return batches;
    }
    for (qsizetype offset = 0; offset < sessions.size(); offset += maximumHeartbeatSessions) {
        QJsonArray batch;
        const qsizetype end = qMin(offset + maximumHeartbeatSessions, sessions.size());
        for (qsizetype index = offset; index < end; ++index) {
            batch.append(sessions.at(index));
        }
        batches.append(batch);
    }
    return batches;
}

/// Sessions a heartbeat reply says the relay would not take, such as ones another enrollment
/// of this Mac still owns. An older relay that omits the list refuses nothing.
inline QSet<QString> heartbeatRefusedSessions(const QJsonValue& reply) {
    QSet<QString> refused;
    for (const QJsonValue& value :
         reply.toObject().value(QStringLiteral("rejectedSessions")).toArray()) {
        const QString id = value.toObject().value(QStringLiteral("sessionId")).toString();
        if (!id.isEmpty())
            refused.insert(id);
    }
    return refused;
}

} // namespace ketplus::daemon
