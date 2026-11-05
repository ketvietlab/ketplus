#pragma once

#include <QVariantMap>
#include <QVector>

#include <limits>

namespace ketplus::transcript_window {

inline constexpr int maximumMessages = 300;
// A window is sized to show about this many viewports, within these bounds.
// Messages that collapse into one grouped row take almost no height, so a
// fixed count can be two screens in one conversation and twenty in another.
inline constexpr double viewportsPerWindow = 15.0;
inline constexpr int largestMessageBudget = 600;
// While the reader scrolls, pages are only ever added: trimming the far edge
// then would keep the window from growing ahead of them. The window is trimmed
// back to its budget once scrolling stops; this bound only stops a very long
// uninterrupted scroll from growing it without limit. A single tool-heavy turn
// can hold thousands of items and over ten megabytes, so both bounds sit well
// above what one turn needs.
inline constexpr int scrollingMessageLimit = 20000;
inline constexpr qsizetype scrollingUtf8Bytes = 96 * 1024 * 1024;
// Hard source-memory bound: rows inside an oversized turn are trimmed from the
// evicted edge. Only the message at the retained edge may exceed it; the rows
// kept beside that message are then trimmed to oversizedCompanionBytes. Its
// presentation is still bounded by AgentMessageModel block segmentation.
inline constexpr qsizetype maximumUtf8Bytes = 16 * 1024 * 1024;
inline constexpr qsizetype oversizedCompanionBytes = 1024 * 1024;
// Passed as a byte limit to bound the count only; the window is then not sized.
inline constexpr qsizetype unboundedUtf8Bytes = std::numeric_limits<qsizetype>::max();

// The idle trim keeps at least this many viewports of rendered rows on each
// side of the reader.
inline constexpr double trimMarginViewports = 12.0;

enum class RetainedEdge { Oldest, Newest };

struct TrimResult final {
    bool changed{false};
    bool removedOlder{false};
    bool removedNewer{false};
};

[[nodiscard]] qsizetype sourceBytes(const QVariantMap& message);
[[nodiscard]] TrimResult trim(QVector<QVariantMap>& messages, RetainedEdge retainedEdge,
                              int messageLimit = maximumMessages,
                              qsizetype utf8ByteLimit = maximumUtf8Bytes,
                              const QString& readingKey = {});
// Rows to evict from each edge so that `messages` fits utf8ByteLimit without
// touching [protectFrom, protectTo), taking from whichever unprotected side
// holds more bytes. Newer rows are taken only when evictNewer is set. The
// window may stay over the limit when the protected rows alone exceed it.
struct ByteCut final {
    int fromOldest{0};
    int fromNewest{0};
};
[[nodiscard]] ByteCut byteCut(const QVector<QVariantMap>& messages, int protectFrom, int protectTo,
                              bool evictNewer, qsizetype utf8ByteLimit = maximumUtf8Bytes);
// Messages needed to fill viewportsPerWindow viewports when the loaded messages
// render renderedHeight in total, clamped to [maximumMessages, largestMessageBudget].
[[nodiscard]] int messageBudget(double renderedHeight, int loadedMessages, double viewportHeight);

} // namespace ketplus::transcript_window
