#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <optional>

namespace ketplus::daemon {
// Payload transformation; adds a timestamped marker. The journal owns age/ACK/turn eligibility and
// transactions.
std::optional<QJsonObject> compactCompletedReplayItem(const QJsonObject& item,
                                                      const QDateTime& cutoff);
} // namespace ketplus::daemon
