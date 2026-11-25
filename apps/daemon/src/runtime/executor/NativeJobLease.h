#pragma once

#include <QtGlobal>

namespace ketplus::daemon::executor {
#ifdef Q_OS_MACOS
inline constexpr qint64 NativeJobLeaseMs = 15000;
#else
inline constexpr qint64 NativeJobLeaseMs = 5000;
#endif
// After the daemon signalled a lost executor's processes, how long it waits for them to
// exit before declaring the loss unverified, so a normal stop does not raise needs_attention.
inline constexpr qint64 NativeJobRecoverySettleMs = 3500;
} // namespace ketplus::daemon::executor
