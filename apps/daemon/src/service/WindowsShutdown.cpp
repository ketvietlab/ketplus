#include "service/WindowsShutdown.h"
#if defined(Q_OS_WIN)
#include <QCoreApplication>
#include <QMetaObject>
#include <qt_windows.h>

namespace ketplus::daemon {
namespace {
BOOL WINAPI onConsoleEvent(DWORD event) {
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        // Runs on a system thread; a queued call is the thread-safe way to reach the loop.
        if (QCoreApplication* application = QCoreApplication::instance())
            QMetaObject::invokeMethod(application, &QCoreApplication::quit, Qt::QueuedConnection);
        return TRUE;
    default:
        return FALSE;
    }
}
} // namespace

WindowsShutdown::WindowsShutdown(QObject* parent)
    : QObject(parent), ready_(::SetConsoleCtrlHandler(onConsoleEvent, TRUE) != 0) {}

WindowsShutdown::~WindowsShutdown() {
    if (ready_)
        ::SetConsoleCtrlHandler(onConsoleEvent, FALSE);
}
} // namespace ketplus::daemon
#endif
