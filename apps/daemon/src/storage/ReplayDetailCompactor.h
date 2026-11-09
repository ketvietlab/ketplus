#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <functional>
#include <optional>

namespace ketplus::daemon {
// Payload transformation; adds a timestamped marker. The journal owns age/ACK/turn eligibility and
// transactions.
// Stores a bounded preview of an image whose pixels are compacted away and returns its media
// reference block, or an empty object when there is none (caption only).
using ImagePreview = std::function<QJsonObject(const QByteArray& image)>;
std::optional<QJsonObject> compactCompletedReplayItem(const QJsonObject& item,
                                                      const QDateTime& cutoff,
                                                      const ImagePreview& preview = {});
} // namespace ketplus::daemon
