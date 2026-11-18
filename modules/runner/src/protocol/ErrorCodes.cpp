#include "protocol/ErrorCodes.h"

#include <array>

namespace ketplus::runner::protocol {
namespace {

struct Entry {
    ErrorCode code;
    QLatin1StringView name;
    ErrorFrame frame;
};

constexpr std::array<Entry, 16> kEntries{{
    {ErrorCode::NotPaired, QLatin1StringView("not_paired"), ErrorFrame::JobRejected},
    {ErrorCode::JobMismatch, QLatin1StringView("job_mismatch"), ErrorFrame::JobRejected},
    {ErrorCode::AlreadyDispatched, QLatin1StringView("already_dispatched"),
     ErrorFrame::JobRejected},
    {ErrorCode::LabelMismatch, QLatin1StringView("label_mismatch"), ErrorFrame::JobRejected},
    {ErrorCode::RunnerBusy, QLatin1StringView("runner_busy"), ErrorFrame::JobRejected},
    {ErrorCode::SpecInvalid, QLatin1StringView("spec_invalid"), ErrorFrame::JobRejected},
    {ErrorCode::ContainmentUnavailable, QLatin1StringView("containment_unavailable"),
     ErrorFrame::JobRejected},
    {ErrorCode::UnsupportedFrame, QLatin1StringView("unsupported_frame"), ErrorFrame::JobRejected},
    {ErrorCode::LeaseLost, QLatin1StringView("lease_lost"), ErrorFrame::JobRejected},
    {ErrorCode::RunnerUnavailable, QLatin1StringView("runner_unavailable"),
     ErrorFrame::JobRejected},
    {ErrorCode::BaseMissing, QLatin1StringView("base_missing"), ErrorFrame::SnapshotError},
    {ErrorCode::SnapshotTooLarge, QLatin1StringView("snapshot_too_large"),
     ErrorFrame::SnapshotError},
    {ErrorCode::ChunkCorrupt, QLatin1StringView("chunk_corrupt"), ErrorFrame::SnapshotError},
    {ErrorCode::PackInvalid, QLatin1StringView("pack_invalid"), ErrorFrame::SnapshotError},
    {ErrorCode::DiskFull, QLatin1StringView("disk_full"), ErrorFrame::SnapshotError},
    {ErrorCode::CheckoutFailed, QLatin1StringView("checkout_failed"), ErrorFrame::SnapshotError},
}};

constexpr std::array<ErrorCode, kEntries.size()> kCodes = [] {
    std::array<ErrorCode, kEntries.size()> codes{};
    for (std::size_t i = 0; i < kEntries.size(); ++i)
        codes[i] = kEntries[i].code;
    return codes;
}();

const Entry& entry(const ErrorCode code) {
    for (const Entry& candidate : kEntries) {
        if (candidate.code == code)
            return candidate;
    }
    Q_UNREACHABLE_RETURN(kEntries.front());
}

} // namespace

std::span<const ErrorCode> allErrorCodes() { return kCodes; }

QLatin1StringView toString(const ErrorCode code) { return entry(code).name; }

std::optional<ErrorCode> errorCodeFromString(const QStringView text) {
    for (const Entry& candidate : kEntries) {
        if (text == candidate.name)
            return candidate.code;
    }
    return std::nullopt;
}

ErrorFrame errorFrame(const ErrorCode code) { return entry(code).frame; }

} // namespace ketplus::runner::protocol
