#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace ketplus::daemon::protocol {

inline constexpr int currentVersion = 0;
// Additive revisions within `currentVersion` (ADR 0015 step 5). A client sends the newest minor
// it speaks in daemon.hello; both sides use the lower of the two.
inline constexpr int currentMinor = 1;
inline constexpr qsizetype maximumFrameBytes = 1024 * 1024;
inline constexpr qsizetype maximumQueuedBytes = 2 * maximumFrameBytes;
inline constexpr qsizetype maximumEventPayloadBytes = 256 * 1024;
inline constexpr qsizetype maximumReplayPayloadBytes = 768 * 1024;
inline constexpr int maximumReplayEvents = 500;

struct ValidationResult final {
    bool valid{false};
    QString errorCode;
    QString message;
};

[[nodiscard]] ValidationResult validateEnvelope(const QJsonObject& envelope);
[[nodiscard]] QByteArray encodeFrame(const QJsonObject& envelope);
// What goes out instead of a message whose frame would exceed maximumFrameBytes, so the
// client keeps its connection (ADR 0115). A reply becomes a `response_too_large` failure
// with the same correlation; an event keeps its scalar fields (ids, revision) and says
// `truncated: true`, and the client reads the full state again with a request.
[[nodiscard]] QJsonObject oversizedReplacement(const QJsonObject& envelope, qsizetype bytes);
[[nodiscard]] QJsonObject response(const QJsonObject& request, const QJsonObject& payload);
[[nodiscard]] QJsonObject failure(const QJsonObject& request, const QString& code,
                                  const QString& message);
[[nodiscard]] QString timestamp();
// The minor both sides speak; a client that sends none speaks minor 0.
[[nodiscard]] int negotiatedMinor(const QJsonObject& helloPayload);
// Optional behaviour this daemon offers at `minor`, as "name" or "name/revision".
// `schedules/1` (ADR 0126) is the schedule.* requests and events in ScheduleProtocol.h.
[[nodiscard]] QJsonArray features(int minor);

} // namespace ketplus::daemon::protocol
