#pragma once

#include <QLatin1StringView>
#include <QString>

#include <optional>
#include <span>

// Codes carried by `job.rejected` and `snapshot.error`.
namespace ketplus::runner::protocol {

enum class ErrorCode {
    // job.rejected
    NotPaired,
    JobMismatch,
    AlreadyDispatched,
    LabelMismatch,
    RunnerBusy,
    SpecInvalid,
    ContainmentUnavailable,
    UnsupportedFrame,
    LeaseLost,
    RunnerUnavailable,
    // snapshot.error
    BaseMissing,
    SnapshotTooLarge,
    ChunkCorrupt,
    PackInvalid,
    DiskFull,
    CheckoutFailed,
};

// The frame type a code belongs to.
enum class ErrorFrame { JobRejected, SnapshotError };

[[nodiscard]] std::span<const ErrorCode> allErrorCodes();
[[nodiscard]] QLatin1StringView toString(ErrorCode code);
[[nodiscard]] std::optional<ErrorCode> errorCodeFromString(QStringView text);
[[nodiscard]] ErrorFrame errorFrame(ErrorCode code);

} // namespace ketplus::runner::protocol
