#pragma once

#include <QCoreApplication>

namespace ketplus::daemon {

// The application object of the daemon and of its job executor.
//
// On macOS a QCoreApplication runs QEventDispatcherUNIX, which never drains an Objective-C
// autorelease pool. Qt and the system frameworks still return autoreleased objects (for example
// QStandardPaths::writableLocation through NSSearchPathForDirectoriesInDomains), so without a
// pool of our own each such call stays in memory until the process exits. notify() delivers every
// event of every Qt thread, so each delivery gets a pool there.
class DaemonApplication final : public QCoreApplication {
  public:
    using QCoreApplication::QCoreApplication;

    // Adds the bundle's Contents/PlugIns to the Qt plugin search path. Without it a helper in
    // Contents/Helpers cannot load the bundled QSQLITE driver, since the bundle has no qt.conf.
    // Call it after constructing the application object. A no-op outside macOS.
    static void addBundledPluginPath();

#if defined(Q_OS_MACOS)
    bool notify(QObject* receiver, QEvent* event) override;
#endif
};

#if !defined(Q_OS_MACOS)
inline void DaemonApplication::addBundledPluginPath() {}
#endif

} // namespace ketplus::daemon
