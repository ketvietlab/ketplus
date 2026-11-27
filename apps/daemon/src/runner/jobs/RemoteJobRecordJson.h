#pragma once

#include "runner/jobs/RemoteJobRecord.h"

#include <QByteArray>

// The stored form of a remote job record: one versioned JSON object. The revision is kept
// beside it by the store, not inside it.
namespace ketplus::daemon::runner::jobs {

[[nodiscard]] QByteArray encodeRecord(const RemoteJobRecord& record);
// nullopt when the bytes are not a record of this version with known values.
[[nodiscard]] std::optional<RemoteJobRecord> decodeRecord(const QByteArray& bytes);

} // namespace ketplus::daemon::runner::jobs
