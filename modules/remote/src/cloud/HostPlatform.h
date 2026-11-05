#pragma once
#include <QString>
namespace ketplus::daemon {
inline QString hostPlatform() {
#if defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#else
    return QStringLiteral("unsupported");
#endif
}
} // namespace ketplus::daemon
