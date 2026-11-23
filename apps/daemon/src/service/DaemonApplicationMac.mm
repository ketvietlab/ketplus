#include "service/DaemonApplication.h"

#include <QDir>

#import <Foundation/Foundation.h>

namespace ketplus::daemon {

void DaemonApplication::addBundledPluginPath() {
    const QDir executableDirectory(QCoreApplication::applicationDirPath());
    const QString pluginPath =
        QDir(executableDirectory.filePath(QStringLiteral("../PlugIns"))).canonicalPath();
    if (!pluginPath.isEmpty())
        QCoreApplication::addLibraryPath(pluginPath);
}

// The pool is scoped to one delivery, so it stays balanced when a handler runs a nested event
// loop: the inner deliveries get their own pools and this one is popped after the handler returns.
bool DaemonApplication::notify(QObject* receiver, QEvent* event) {
    @autoreleasepool {
        return QCoreApplication::notify(receiver, event);
    }
}

} // namespace ketplus::daemon
