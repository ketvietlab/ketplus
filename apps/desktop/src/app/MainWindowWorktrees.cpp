#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "app/AgentWorktreeCreator.h"
#include "app/MainWindow.h"
#include "app/SettingsWorkspacePage.h"
#include "editor/EditorWidget.h"
#include "git/GitChangesPanel.h"
#include "git/GitService.h"
#include "terminal/TerminalPanel.h"
#include "ui/Theme.h"
#include "workspace/ExplorerPanel.h"

#include <QAction>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLayout>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QUrl>

#include <algorithm>

namespace ketplus {
void MainWindow::hideUtilityWorkspaces() {
    if (settingsWorkspace_ != nullptr) {
        auto* page = settingsWorkspace_;
        settingsWorkspace_ = nullptr;
        settingsDialog_ = nullptr;
        page->hide();
        page->deleteLater();
    }
    if (settingsWorkspaceMode_)
        theme_.cancelPreview();
    settingsWorkspaceMode_ = false;
}

QString MainWindow::worktreeRemovalBlocker(const QString& target) const {
    if (target.isEmpty() || !QFileInfo(QDir(target).filePath(QStringLiteral(".git"))).isFile())
        return tr("it is not a linked worktree");
    const auto contains = [&target](const QString& file) {
        const QString relative = QDir(target).relativeFilePath(file);
        return !relative.isEmpty() && !QDir::isAbsolutePath(relative) &&
               relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../"));
    };
    QStringList reasons;
    for (const EditorWidget* editor : findChildren<EditorWidget*>()) {
        if (editor->document().isModified() && contains(editor->document().filePath())) {
            reasons.append(tr("an editor has unsaved changes in it"));
            break;
        }
    }
    // A conversation can run in a checkout other than the one it was started in.
    if (agentManager_->runningCheckoutPaths().contains(target))
        reasons.append(tr("an agent is running in it"));
    return reasons.join(QStringLiteral("; "));
}

void MainWindow::showAgentWorktreeContextMenu(const QString& path) {
    showWorktreeMenu(path, QCursor::pos());
}

void MainWindow::showWorktreeMenu(const QString& path, const QPoint& globalPosition) {
    const auto target = QFileInfo(path).canonicalFilePath();
    bool known = false;
    for (const auto& tree : worktrees_)
        if (!tree.bare && !tree.prunable && QFileInfo(tree.path).canonicalFilePath() == target)
            known = true;
    if (!known || target.isEmpty())
        return;
    const auto canRemove = [this, target] {
        return worktreeRemovalBlocker(target).isEmpty() &&
               target != QFileInfo(workspaceRoot_).canonicalFilePath() &&
               !agentWorktreeCreator_->isRunning();
    };
    // The mock's manage menu: where to go with the worktree, then what ends it.
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("worktreeManageMenu"));
    auto* converse = menu.addAction(tr("New conversation here"));
    converse->setObjectName("newConversationInWorktreeAction");
    converse->setEnabled(agentManager_ != nullptr);
    auto* openFiles = menu.addAction(tr("Open in Files && Editor"));
    openFiles->setObjectName("openWorktreeFilesAction");
#ifdef Q_OS_MACOS
    auto* reveal = menu.addAction(tr("Reveal in Finder"));
#else
    auto* reveal = menu.addAction(tr("Show in file manager"));
#endif
    reveal->setObjectName("revealWorktreeAction");
    reveal->setToolTip(QDir::toNativeSeparators(target));
    menu.setToolTipsVisible(true);
    menu.addSeparator();
    auto* archive = menu.addAction(tr("Archive worktree"));
    archive->setObjectName("archiveWorktreeAction");
    archive->setEnabled(target != QFileInfo(workspaceRoot_).canonicalFilePath());
    auto* remove = menu.addAction(tr("Delete worktree…"));
    remove->setObjectName("deleteWorktreeAction");
    remove->setEnabled(canRemove());
    if (!remove->isEnabled()) {
        auto* reason =
            menu.addAction(tr("Main, running, locked, dirty, or worktrees with unsaved editors "
                              "cannot be deleted."));
        reason->setEnabled(false);
    }
    QAction* selected = menu.exec(globalPosition);
    if (selected == converse) {
        promotedWorktreePath_ = target;
        switchToWorktree(target);
        setAgentWorkspaceVisible(true);
        agentManager_->newSession(target);
        return;
    }
    if (selected == openFiles) {
        switchToWorktree(target);
        setExplorerVisible(true);
        return;
    }
    if (selected == reveal) {
#ifdef Q_OS_MACOS
        QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), target});
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(target));
#endif
        return;
    }
    if (selected == archive) {
        if (archiveWorktree(target)) {
            statusBar()->showMessage(tr("Worktree archived. Restore it from Settings."), 5000);
        }
        return;
    }
    if (selected != remove || !canRemove())
        return;
    const auto confirmation = QMessageBox::warning(
        this, tr("Delete worktree"),
        tr("Permanently delete this linked worktree directory?\n%1\n\nThe branch and archived "
           "conversation history will be kept. Git will refuse dirty, untracked, or locked "
           "worktrees.")
            .arg(QDir::toNativeSeparators(target)),
        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
    if (confirmation != QMessageBox::Ok)
        return;
    if (canRemove())
        agentWorktreeCreator_->remove(workspaceRoot_, target);
}

namespace {
QString worktreeIdentity(const QString& path) {
    const QFileInfo marker(QDir(path).filePath(QStringLiteral(".git")));
    QString gitDirectory;
    if (marker.isDir()) {
        gitDirectory = marker.canonicalFilePath();
    } else if (marker.isFile()) {
        QFile file(marker.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
            return {};
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (!line.startsWith(QStringLiteral("gitdir: ")))
            return {};
        const QString candidate = line.mid(8).trimmed();
        gitDirectory = QFileInfo(candidate).isAbsolute()
                           ? QFileInfo(candidate).canonicalFilePath()
                           : QFileInfo(QDir(path).filePath(candidate)).canonicalFilePath();
    }
    if (gitDirectory.isEmpty())
        return {};
    const QDateTime created = QFileInfo(gitDirectory).birthTime();
    return gitDirectory + QLatin1Char('|') +
           (created.isValid() ? QString::number(created.toMSecsSinceEpoch()) : QStringLiteral("0"));
}

QStringList archivedWorktreePaths() {
    return QSettings().value(QStringLiteral("agent/archivedWorktrees")).toStringList();
}

void saveArchivedWorktreePaths(QStringList values) {
    values.removeDuplicates();
    values.sort(Qt::CaseInsensitive);
    QSettings().setValue(QStringLiteral("agent/archivedWorktrees"), values);
}

QVariantMap archivedWorktreeIdentities() {
    return QSettings().value(QStringLiteral("agent/archivedWorktreeIdentities")).toMap();
}

void saveArchivedWorktreeIdentities(const QVariantMap& identities) {
    QSettings().setValue(QStringLiteral("agent/archivedWorktreeIdentities"), identities);
}
} // namespace

bool MainWindow::archiveWorktree(const QString& path) {
    const QString target = QFileInfo(path).canonicalFilePath();
    if (target.isEmpty() || target == QFileInfo(workspaceRoot_).canonicalFilePath()) {
        return false;
    }
    QStringList values = archivedWorktreePaths();
    if (!values.contains(target)) {
        values.append(target);
        saveArchivedWorktreePaths(values);
    }
    QVariantMap identities = archivedWorktreeIdentities();
    identities.insert(target, worktreeIdentity(target));
    saveArchivedWorktreeIdentities(identities);
    refreshAgentWorktrees();
    return true;
}

bool MainWindow::restoreArchivedWorktree(const QString& path) {
    QStringList values = archivedWorktreePaths();
    const QString target = QFileInfo(path).canonicalFilePath();
    if (target.isEmpty() || !QFileInfo(target).isDir())
        return false;
    const bool registered =
        std::any_of(worktrees_.cbegin(), worktrees_.cend(), [&target](const auto& tree) {
            return !tree.bare && !tree.prunable &&
                   QFileInfo(tree.path).canonicalFilePath() == target;
        });
    if (!registered)
        return false;
    QVariantMap identities = archivedWorktreeIdentities();
    const QString expectedIdentity = identities.value(target).toString();
    if (!expectedIdentity.isEmpty() && expectedIdentity != worktreeIdentity(target))
        return false;
    const int removed = values.removeAll(target);
    if (removed == 0) {
        return false;
    }
    saveArchivedWorktreePaths(values);
    identities.remove(target);
    saveArchivedWorktreeIdentities(identities);
    refreshAgentWorktrees();
    return true;
}

QVariantList MainWindow::archivedWorktrees() const {
    QVariantList result;
    for (const QString& path : archivedWorktreePaths()) {
        result.append(QVariantMap{
            {QStringLiteral("id"), QString{}},
            {QStringLiteral("path"), path},
            {QStringLiteral("title"), QFileInfo(path).fileName()},
            {QStringLiteral("kind"), QStringLiteral("worktree")},
            {QStringLiteral("running"),
             agentManager_ != nullptr &&
                 agentManager_->runningPaths().contains(QFileInfo(path).canonicalFilePath())}});
    }
    return result;
}
} // namespace ketplus
