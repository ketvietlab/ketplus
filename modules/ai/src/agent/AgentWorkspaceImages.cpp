#include "KetPrint.h"
#include "agent/AgentWorkspaceView.h"
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QStandardPaths>

namespace ketplus {

QString AgentWorkspaceView::imageCaption(const QUrl& source) const {
    const QString name =
        QFileInfo(source.isLocalFile() ? source.toLocalFile() : source.path()).fileName();
    return name.isEmpty() ? tr("Image") : name;
}

bool AgentWorkspaceView::copyImage(const QUrl& source, int rotation) {
    const QImage image = ket::printableImage(source, rotation);
    if (image.isNull() || QGuiApplication::clipboard() == nullptr)
        return false;
    QGuiApplication::clipboard()->setImage(image);
    return true;
}

QString AgentWorkspaceView::saveImageToDownloads(const QUrl& source) {
    const QDir downloads(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    if (!downloads.exists())
        return {};
    // A file keeps its bytes and name; anything else is written out as a PNG.
    const bool local = source.isLocalFile();
    const QFileInfo original(local ? source.toLocalFile() : QString());
    const QString base = local ? original.completeBaseName() : QStringLiteral("image");
    const QString suffix =
        local && !original.suffix().isEmpty() ? original.suffix() : QStringLiteral("png");
    QString target = downloads.filePath(base + QLatin1Char('.') + suffix);
    for (int copy = 2; QFileInfo::exists(target); ++copy)
        target = downloads.filePath(QStringLiteral("%1 (%2).%3").arg(base).arg(copy).arg(suffix));
    if (local)
        return QFile::copy(original.filePath(), target) ? target : QString();
    const QImage image = ket::printableImage(source, 0);
    return !image.isNull() && image.save(target, "PNG") ? target : QString();
}

bool AgentWorkspaceView::printImage(const QUrl& source, int rotation, const QString& title) {
    return ket::printImage(source, rotation, title, this);
}

void AgentWorkspaceView::toggleWindowFullScreen() {
    QWidget* top = window();
    top->setWindowState(top->windowState() ^ Qt::WindowFullScreen);
}

} // namespace ketplus
