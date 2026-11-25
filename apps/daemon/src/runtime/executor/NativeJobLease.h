#pragma once

#include <QtGlobal>

namespace ketplus::daemon::executor {
#ifdef Q_OS_MACOS
inline constexpr qint64 NativeJobLeaseMs = 15000;
#else
inline constexpr qint64 NativeJobLeaseMs = 5000;
#endif
} // namespace ketplus::daemon::executor
