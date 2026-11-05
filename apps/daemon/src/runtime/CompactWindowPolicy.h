#pragma once

#include <QJsonArray>
#include <QJsonObject>

namespace ketplus::daemon::context_harness {

inline constexpr qint64 minimumCompactWindow = 100'000;
inline constexpr qint64 maximumCompactWindow = 1'000'000;

// Undefined inherits the session request; null restores provider-owned defaults.
// This is an auto-compaction request, never the model's physical context capacity.
// Background tasks retain their submitted settings and do not lock future defaults.
[[nodiscard]] QJsonObject resolveCompactWindow(const QJsonValue& value, const QString& provider,
                                               qint64 inherited, const QJsonArray& backgroundTasks,
                                               qint64& resolved);
[[nodiscard]] QJsonValue compactWindowJson(qint64 tokens);
[[nodiscard]] QJsonObject codexCompactConfig(qint64 tokens);

} // namespace ketplus::daemon::context_harness
