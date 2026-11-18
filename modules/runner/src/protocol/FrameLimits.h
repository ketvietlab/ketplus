#pragma once

#include <QtGlobal>

// Field limits of the runner peer frames.
namespace ketplus::runner::protocol::limits {

inline constexpr qsizetype KiB = 1024;

// Frame sizes, in bytes of the UTF-8 JSON message.
inline constexpr qsizetype maxFrameBytes = 64 * KiB;
inline constexpr qsizetype maxFrameBytes16K = 16 * KiB;
inline constexpr qsizetype maxFrameBytes4K = 4 * KiB;
inline constexpr qsizetype maxFrameBytes1K = 1 * KiB;

inline constexpr qsizetype maxArgv = 256;
inline constexpr qsizetype maxShellBytes = 32 * KiB;
inline constexpr qsizetype maxEnvNames = 64;
inline constexpr qsizetype maxEnvBytes = 32 * KiB; // names plus values, UTF-8
inline constexpr qsizetype maxCwdBytes = 1024;
inline constexpr qint64 minTimeoutMs = 1000;
inline constexpr qint64 maxTimeoutMs = 24LL * 60 * 60 * 1000;

inline constexpr qsizetype maxWarnings = 8;   // entries received, duplicates included
inline constexpr qsizetype maxCodeChars = 64; // `code`, `reason` and each warning
inline constexpr qsizetype maxSnapshots = 256;
inline constexpr qsizetype maxIdChars = 128; // ids and versions
inline constexpr qsizetype maxRunnerVersionChars = 64;
inline constexpr qsizetype maxNonceChars = 128;
inline constexpr qsizetype maxLabels = 64;
inline constexpr qsizetype maxLabelChars = 64;
inline constexpr qsizetype maxCapabilities = 32;
inline constexpr qsizetype maxCapabilityChars = 64;

// The largest pack a runner accepts. `snapshot.offer` may announce more; the runner answers
// such an offer with `snapshot.error` `snapshot_too_large` and the desktop never offers one.
inline constexpr qint64 maxPackBytes = 4LL * 1024 * 1024 * 1024;
inline constexpr qint64 chunkBytes = 4LL * 1024 * 1024;
inline constexpr qsizetype maxOutputBytes = 32 * KiB; // raw bytes in one `job.output`

// Integers travel as JSON numbers; larger values lose precision in a double.
inline constexpr qint64 maxSafeInteger = (1LL << 53) - 1;
inline constexpr qint64 maxAttempt = (1LL << 31) - 1;
inline constexpr qint64 minExitCode = -(1LL << 31);
inline constexpr qint64 maxExitCode = (1LL << 31) - 1;
inline constexpr qint64 maxSignal = 64;

[[nodiscard]] constexpr bool packTooLarge(const qint64 packBytes) {
    return packBytes > maxPackBytes;
}

} // namespace ketplus::runner::protocol::limits
