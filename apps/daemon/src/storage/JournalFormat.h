#pragma once

#include <QString>

namespace ketplus::daemon::journal_format {

// The journal's PRAGMA user_version: the format this daemon writes and the newest it opens
// (ADR 0095). Raise it whenever an older daemon would misread or damage what a newer one wrote.
// Journals from before versioning read 0 and are upgraded on open.
inline constexpr int current = 1;

[[nodiscard]] constexpr bool supported(const int version) {
    return version >= 0 && version <= current;
}

// Reads the format of an existing journal through a read-only connection, before anything
// opens it for writing or cleans up beside it. A read-only close never checkpoints or truncates
// the write-ahead log, which may hold the newest format. A missing journal is Supported; one the
// read-only connection cannot read is left to the caller's usual open and its errors.
enum class Status { Supported, Unsupported, Unreadable };
Status check(const QString& path, QString* error);

} // namespace ketplus::daemon::journal_format
