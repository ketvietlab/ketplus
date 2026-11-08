#pragma once

namespace ketplus::daemon::journal_format {

// The journal's PRAGMA user_version: the format this daemon writes and the newest it opens
// (ADR 0095). Raise it whenever an older daemon would misread or damage what a newer one wrote.
// Journals from before versioning read 0 and are upgraded on open.
inline constexpr int current = 1;

} // namespace ketplus::daemon::journal_format
