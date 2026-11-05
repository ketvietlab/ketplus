#include "agent/AgentSessionManager.h"
#include "app/AgentWorktreeCreator.h"
#include "app/MainWindow.h"
#include "app/SettingsWorkspacePage.h"
#include "app/WorktreesWorkspacePage.h"
#include "agent/AgentWorkspaceView.h"
#include "editor/EditorWidget.h"
#include "git/GitChangesPanel.h"
#include "git/GitService.h"
#include "terminal/TerminalPanel.h"
#include "ui/Theme.h"
#include "workspace/ExplorerPanel.h"

#include <QAction>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QLayout>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QSplitter>
#include <QTabWidget>

#include <algorithm>

namespace ketplus {
void MainWindow::hideUtilityWorkspaces() {
    if (worktreesWorkspace_ != nullptr)
        worktreesWorkspace_->hide();
    worktreesWorkspaceMode_ = false;
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

void MainWindow::refreshWorktreesWorkspace() {
    if (worktreesWorkspace_ == nullptr)
        return;
    QSet<QString> archivedPaths;
    for (const QVariant& value : archivedWorktrees()) {
        const QString path = QFileInfo(value.toMap().value(QStringLiteral("path")).toString())
                                 .canonicalFilePath();
        if (!path.isEmpty())
            archivedPaths.insert(path);
    }
    QVector<GitWorktree> visible;
    QHash<QString, int> conversations;
    bool hasPrunableWorktrees = false;
    for (const GitWorktree& worktree : std::as_const(worktrees_)) {
        hasPrunableWorktrees = hasPrunableWorktrees ||
                               (!worktree.bare &&
                                (worktree.prunable || !QFileInfo(worktree.path).isDir()));
        const QString path = QFileInfo(worktree.path).canonicalFilePath();
        if (worktree.bare || worktree.prunable || archivedPaths.contains(path))
            continue;
        visible.append(worktree);
        conversations.insert(path,
                             agentManager_ == nullptr ? 0
                                                      : agentManager_->conversations(path).size());
    }
    worktreesWorkspace_->setWorktrees(
        visible, workspaceRoot_,
        agentManager_ == nullptr ? QSet<QString>{} : agentManager_->runningPaths(), conversations,
        !gitSnapshot_.files.isEmpty());
    worktreesWorkspace_->setPruneAvailable(hasPrunableWorktrees &&
                                           !agentWorktreeCreator_->isRunning());
}

void MainWindow::setWorktreesWorkspaceVisible(const bool visible) {
    if (!visible) {
        if (worktreesWorkspace_ != nullptr)
            worktreesWorkspace_->hide();
        worktreesWorkspaceMode_ = false;
        return;
    }

    hideUtilityWorkspaces();
    setRemoteWorkspaceVisible(false);
    setAtlasWorkspaceVisible(false);
    setAgentWorkspaceVisible(false);
    ensureAgentWorkspace();
    if (worktreesWorkspace_ == nullptr) {
        worktreesWorkspace_ = new WorktreesWorkspacePage(workspaceSplit_);
        workspaceSplit_->addWidget(worktreesWorkspace_);
        worktreesWorkspace_->applyPalette(theme_.palette(), theme_.interfaceFontSizePixels());
        connect(worktreesWorkspace_, &WorktreesWorkspacePage::createRequested, this,
                &MainWindow::createAgentWorktree);
        connect(worktreesWorkspace_, &WorktreesWorkspacePage::pruneRequested, this, [this] {
            if (agentWorktreeCreator_ == nullptr || agentWorktreeCreator_->isRunning())
                return;
            worktreesWorkspace_->setPruning(true);
            agentWorktreeCreator_->prune(workspaceRoot_);
        });
        connect(worktreesWorkspace_, &WorktreesWorkspacePage::worktreeRequested, this,
                [this](const QString& path) {
                    promotedWorktreePath_ = QFileInfo(path).canonicalFilePath();
                    switchToWorktree(path);
                    setAgentWorkspaceVisible(true);
                });
        connect(worktreesWorkspace_, &WorktreesWorkspacePage::manageRequested, this,
                &MainWindow::showAgentWorktreeContextMenu);
        connect(worktreesWorkspace_, &WorktreesWorkspacePage::archivedRequested, this,
                &MainWindow::showSettings);
    }
    if (explorer_ != nullptr)
        explorer_->hide();
    if (gitChanges_ != nullptr)
        gitChanges_->hide();
    if (terminal_ != nullptr)
        terminal_->hide();
    if (agentWorkspace_ != nullptr)
        agentWorkspace_->hide();
    if (agentDetailPane_ != nullptr)
        agentDetailPane_->hide();
    editorSplit_->hide();
    worktreesWorkspaceMode_ = true;
    settingsWorkspaceMode_ = false;
    worktreesWorkspace_->show();
    refreshWorktreesWorkspace();
    setShellMode(ActivityBar::Mode::AgentWorkspace);
    workspaceSplit_->show();
    mainSplit_->show();
    mainSplit_->setMaximumHeight(QWIDGETSIZE_MAX);
    mainSplit_->parentWidget()->layout()->activate();
    QList<int> shellSizes(mainSplit_->count(), 0);
    shellSizes[mainSplit_->indexOf(workspaceSplit_)] = qMax(480, mainSplit_->height());
    mainSplit_->setSizes(shellSizes);
    QList<int> sizes(workspaceSplit_->count(), 0);
    sizes[workspaceSplit_->indexOf(worktreesWorkspace_)] = qMax(800, workspaceSplit_->width());
    workspaceSplit_->setSizes(sizes);
}

QString MainWindow::worktreeRemovalBlocker(const QString& target) const {
    if (target.isEmpty() || !QFileInfo(QDir(target).filePath(QStringLiteral(".git"))).isFile())
        return tr("it is not a linked worktree");
    const auto contains = [&target](const QString& file) {
        const QString relative = QDir(target).relativeFilePath(file);
        return !relative.isEmpty() && !QDir::isAbsolutePath(relative) &&
               relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../"));
    };
    for (const EditorWidget* editor : findChildren<EditorWidget*>()) {
        if (editor->document().isModified() && contains(editor->document().filePath()))
            return tr("an editor has unsaved changes in it");
    }
    if (agentManager_->runningPaths().contains(target))
        return tr("an agent is running in it");
    return {};
}

void MainWindow::showAgentWorktreeContextMenu(const QString& path) {
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
    QMenu menu(this);
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
    QAction* selected = menu.exec(QCursor::pos());
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
