#include "agent/AgentWorkspaceView.h"
#include "agent/FileReveal.h"
#include "app/MainWindow.h"
#include "app/TranscriptFileType.h"
#include "editor/EditorWidget.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QRegularExpression>
#include <QStatusBar>
#include <QUrl>

namespace ketplus {
namespace {

QString normalizedPath(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo file(path);
    const QString canonical = file.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? file.absoluteFilePath() : canonical);
}

bool transcriptPathInWorktree(const QString& path, const QString& worktree) {
    const QString root = normalizedPath(worktree);
    return !root.isEmpty() && (path == root || path.startsWith(root + QLatin1Char('/')));
}

} // namespace

// Links in transcripts come from model output, so where they lead is decided here and not by
// the platform: text opens in the editor, viewer types open after a check, everything else is
// revealed, and URLs other than web and mail ask first.
void MainWindow::openTranscriptLink(const QString& link) {
    if (isRemoteFileLink(link)) {
        statusBar()->showMessage(
            QStringLiteral("Refused a link to a network location: %1").arg(link), 5000);
        return;
    }
    const QString agentPath =
        agentWorkspace_->activePath().isEmpty() ? workspaceRoot_ : agentWorkspace_->activePath();
    const auto localPath = [&agentPath](const QString& value) {
        const QUrl url(value);
        if (url.isLocalFile())
            return url.toLocalFile();
        if (QDir::isAbsolutePath(value))
            return value;
        if (!url.scheme().isEmpty())
            return QString{};
        const QString decodedPath = url.path(QUrl::FullyDecoded);
        return decodedPath.startsWith(QStringLiteral("~/"))
                   ? QDir::home().filePath(decodedPath.mid(2))
                   : QDir(agentPath).filePath(decodedPath);
    };
    static const QRegularExpression sourceLocation(QStringLiteral(R"(^(.+):(\d+)(?::\d+)?$)"));
    const auto locationMatch = sourceLocation.match(link);
    QString path = locationMatch.hasMatch() ? localPath(locationMatch.captured(1)) : QString{};
    if (path.isEmpty())
        path = localPath(link);
    if (path.isEmpty()) {
        openTranscriptUrl(link);
        return;
    }
    if (isRemoteFileLink(path)) {
        statusBar()->showMessage(
            QStringLiteral("Refused a link to a network location: %1").arg(path), 5000);
        return;
    }
    path = normalizedPath(path);
    const TranscriptFileAction action = transcriptFileAction(path);
    if (action == TranscriptFileAction::Reveal) {
        // Folders, apps and executables are shown, never launched from a link.
        file_reveal::reveal(path);
        statusBar()->showMessage(QStringLiteral("Revealed instead of opening: %1").arg(path), 4000);
    } else if (action == TranscriptFileAction::View) {
        if (!transcriptPathInWorktree(path, agentPath) &&
            QMessageBox::question(this, QStringLiteral("Open file outside the worktree?"),
                                  QStringLiteral("An agent link points outside this worktree:\n\n%1"
                                                 "\n\nOpen it with its default app?")
                                      .arg(QDir::toNativeSeparators(path)),
                                  QMessageBox::Open | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Open)
            return;
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
            statusBar()->showMessage(QStringLiteral("Could not open: %1").arg(link), 3000);
        }
    } else if (action == TranscriptFileAction::Edit) {
        openTranscriptFile(path, false);
        // `file.ts:42` lands on its line.
        if (locationMatch.hasMatch() &&
            path == normalizedPath(localPath(locationMatch.captured(1)))) {
            if (auto* editor = currentEditor())
                editor->goToLine(locationMatch.captured(2).toInt());
        }
    } else {
        statusBar()->showMessage(QStringLiteral("File not found: %1").arg(link), 3000);
    }
}

void MainWindow::openTranscriptUrl(const QUrl& url) {
    switch (transcriptUrlAction(url)) {
    case TranscriptUrlAction::Open:
        QDesktopServices::openUrl(url);
        return;
    case TranscriptUrlAction::Confirm:
        if (QMessageBox::question(this, QStringLiteral("Open link?"),
                                  QStringLiteral("An agent link opens another app:\n\n%1\n\n"
                                                 "Only continue if you expected this link.")
                                      .arg(url.toDisplayString()),
                                  QMessageBox::Open | QMessageBox::Cancel,
                                  QMessageBox::Cancel) == QMessageBox::Open)
            QDesktopServices::openUrl(url);
        return;
    case TranscriptUrlAction::Refuse:
        statusBar()->showMessage(
            QStringLiteral("Refused a link to a network location: %1").arg(url.toDisplayString()),
            5000);
        return;
    }
}

} // namespace ketplus
