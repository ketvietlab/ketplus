#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>

namespace ketplus::daemon {

[[nodiscard]] QJsonObject projectEventPayloadForCloud(const QString& eventType,
                                                      const QJsonObject& payload);

// Plaintext metadata the relay needs to drop streamed text a completed item repeats, since it
// cannot read the encrypted item ids: `itemKey`, an opaque digest of the session, generation and
// item id, on streamed deltas and item completions; and `supersedesDeltas` on a completion whose
// cloud payload carries the item's whole text inline. Empty for every other event.
[[nodiscard]] QJsonObject cloudItemMetadata(const QString& sessionId, qint64 generation,
                                            const QString& eventType, const QJsonObject& payload);

// Given a large-event transfer id, returns the cloud projection of the reassembled payload when
// it differs from the raw payload (or cannot be verified), and nullopt when the raw bytes are
// already cloud-safe and may be served through large-event continuations.
using LargeEventCloudProjection = std::function<std::optional<QJsonObject>(
    const QString& transferId, const QJsonObject& manifest)>;

// Projects a local `session.transcript_window` result for a cloud restore page: every event
// payload goes through projectEventPayloadForCloud; a large-event manifest whose content is not
// cloud-safe is replaced inline by its projection, and its raw chunk events and partial-item
// transfer references are dropped. Local (desktop) windows never go through this.
[[nodiscard]] QJsonObject projectTranscriptWindowForCloud(const QJsonObject& page,
                                                          const LargeEventCloudProjection& large);

// Exposed for tests: the sanitized pieces of the MCP projection.
[[nodiscard]] QString sanitizeCloudUrl(const QString& value);
[[nodiscard]] QString sanitizeCloudText(QString value);
[[nodiscard]] QString cloudArgumentSummary(const QJsonObject& arguments);
[[nodiscard]] QString capCloudText(const QString& value, qsizetype limit);

} // namespace ketplus::daemon
