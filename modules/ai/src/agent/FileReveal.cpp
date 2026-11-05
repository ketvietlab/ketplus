#include "agent/FileReveal.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

#include <utility>

namespace ketplus::file_reveal {
namespace {

std::function<void(const QString&)>& handler() {
    static std::function<void(const QString&)> value;
    return value;
}

} // namespace

void reveal(const QString& absolutePath) {
    if (const auto& replaced = handler()) {
        replaced(absolutePath);
        return;
    }
#if defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), absolutePath});
#elif defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,") + QDir::toNativeSeparators(absolutePath)});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(absolutePath).absolutePath()));
#endif
}

void setHandler(std::function<void(const QString& absolutePath)> replacement) {
    handler() = std::move(replacement);
}

} // namespace ketplus::file_reveal
