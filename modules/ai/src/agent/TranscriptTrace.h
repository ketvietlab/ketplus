#pragma once

#include <QString>
#include <QVariantMap>

namespace ketplus::transcript_trace {

// One process-wide JSONL trace for transcript layout and data events, opened
// from KETPLUS_CHAT_LAYOUT_TRACE. The view's frame records and the model's and
// session's structural events land in the same file in one sequence, so a jump
// on screen can be read next to the mutation that caused it.
//
// Disabled (and free) unless the environment variable names a writable file.
// KETPLUS_CHAT_LAYOUT_TRACE_MAX_MB bounds the file (default 8).
[[nodiscard]] bool enabled();
void write(const QString& event, QVariantMap values);

} // namespace ketplus::transcript_trace
