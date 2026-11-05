#include "terminal_app/TerminalApplication.h"

#include <QEvent>
#include <QFileOpenEvent>
#include <QUrl>

#include <utility>

namespace ketplus {

TerminalApplication::TerminalApplication(int& argc, char** argv) : QApplication(argc, argv) {}

void TerminalApplication::setOpenPathHandler(OpenPathHandler handler) {
    openPathHandler_ = std::move(handler);
    if (!openPathHandler_) {
        return;
    }
    const QStringList pendingPaths = std::exchange(pendingPaths_, {});
    for (const QString& path : pendingPaths) {
        openPathHandler_(path);
    }
}

bool TerminalApplication::event(QEvent* event) {
    if (event->type() == QEvent::FileOpen) {
        const auto* openEvent = static_cast<QFileOpenEvent*>(event);
        QString path = openEvent->file();
        if (path.isEmpty() && openEvent->url().isLocalFile()) {
            path = openEvent->url().toLocalFile();
        }
        if (!path.isEmpty()) {
            if (openPathHandler_) {
                openPathHandler_(path);
            } else {
                pendingPaths_.append(path);
            }
            return true;
        }
    }
    return QApplication::event(event);
}

} // namespace ketplus
