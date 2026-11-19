#include "app/ConversationRepository.h"
#include "app/MainWindow.h"
#include "teams/TeamDirectory.h"

#include "machine/MachineComposition.h"
#include "machine/MachineWorkspaceView.h"

#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "app/RepositoryRouter.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QStatusBar>
#include <QTimer>
#include <QtConcurrentRun>

#include <algorithm>
#include <cstdio>

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

} // namespace

QString MainWindow::activeSidebarRepository() const {
    if (agentWorkspace_ != nullptr && !agentWorkspace_->activePath().isEmpty()) {
        const auto execution = agentWorkspace_->executionPath();
        // A conversation outlives its checkout. Keep its acknowledged repository
        // identity when the linked .git pointer disappears with that checkout.
        const auto remembered = sidebarRepositoryByPath_.value(normalizedPath(execution));
        if (!QFileInfo(execution).isDir() && !remembered.isEmpty())
            return remembered;
        return conversationRepositoryRoot(execution);
    }
    return conversationRepositoryRoot(repositoryDisplayRoot_.isEmpty() ? workspaceRoot_
                                                                       : repositoryDisplayRoot_);
}

void MainWindow::scheduleSidebarRefresh(const bool repositoriesChanged) {
    sidebarRepositoriesStale_ = sidebarRepositoriesStale_ || repositoriesChanged;
    if (sidebarRefreshPending_)
        return;
    sidebarRefreshPending_ = true;
    // Agent activity arrives in bursts; rebuild the sidebar once per event-loop turn.
    QTimer::singleShot(0, this, [this] {
        sidebarRefreshPending_ = false;
        refreshSidebarRepositories();
    });
}

QString MainWindow::sidebarRepositoryForPath(const QString& path) {
    const QString normalized = normalizedPath(path);
    const auto cached = sidebarRepositoryByPath_.constFind(normalized);
    if (cached != sidebarRepositoryByPath_.cend())
        return *cached;
    if (!QFileInfo(normalized).isDir()) {
        // A deleted worktree keeps its conversations but no longer names its repository.
        // Attach it to the repository that contains it or that it was named after
        // ("<repo>/.claude/worktrees/x", "<repo>-feature"); otherwise leave it out.
        QString owner;
        for (const QString& root : std::as_const(sidebarRepositoryRoots_)) {
            const bool nested = normalized.startsWith(root + QLatin1Char('/'));
            const bool sibling =
                QFileInfo(root).absolutePath() == QFileInfo(normalized).absolutePath() &&
                QFileInfo(normalized)
                    .fileName()
                    .startsWith(QFileInfo(root).fileName() + QLatin1Char('-'));
            if ((nested || sibling) && root.size() > owner.size())
                owner = root;
        }
        return owner;
    }
    // Prefer the persisted metadata cache (no Git on the UI thread). The Git worktree list is
    // only a fallback: right after switching repositories it still describes the previous one.
    const QString root =
        conversationRepositoryRoot(RepositoryRouter::cachedRepositoryRoot(normalized));
    if (!root.isEmpty()) {
        sidebarRepositoryByPath_.insert(normalized, root);
        return root;
    }
    // Execution switches already validate an exact Git root. Resolve its .git pointer
    // synchronously (no process) so header/sidebar update in the same event-loop turn.
    if (QFileInfo(QDir(normalized).filePath(".git")).exists()) {
        const auto exactRoot = conversationRepositoryRoot(normalized);
        sidebarRepositoryByPath_.insert(normalized, exactRoot);
        return exactRoot;
    }
    for (const GitWorktree& worktree : std::as_const(worktrees_)) {
        if (normalizedPath(worktree.path) == normalized)
            return activeSidebarRepository();
    }
    // Unknown to both: resolve with Git off the UI thread, then regroup. Without this a
    // repository's conversation count depended on whether that repository was open.
    if (!sidebarRepositoryLookups_.contains(normalized)) {
        sidebarRepositoryLookups_.insert(normalized);
        auto* lookup = new QFutureWatcher<QString>(this);
        connect(lookup, &QFutureWatcher<QString>::finished, this, [this, lookup, normalized] {
            const QString resolved = conversationRepositoryRoot(lookup->result());
            if (!resolved.isEmpty()) {
                sidebarRepositoryByPath_.insert(normalized, resolved);
                scheduleSidebarRefresh();
            }
            lookup->deleteLater();
        });
        lookup->setFuture(QtConcurrent::run(&repositoryLookupPool_, [normalized] {
            return RepositoryRouter::repositoryRoot(normalized);
        }));
    }
    return {};
}

void MainWindow::refreshSidebarRepositories() {
    // KETPLUS_SIDEBAR_TRACE=1: one stderr line per refresh, for the large-workspace benchmark.
    static const bool trace = qEnvironmentVariableIsSet("KETPLUS_SIDEBAR_TRACE");
    QElapsedTimer timer;
    if (trace)
        timer.start();
    if (selectedMachineId_ != QStringLiteral("local")) {
        // ADR 0119: the selected machine's lists, never This Mac's.
        activityBar_->setRepositories(machines_ != nullptr ? machines_->sidebarRepositories()
                                                           : QVector<SidebarRepository>{});
        return;
    }
    const QString activeRepository = activeSidebarRepository();
    if (sidebarRepositoriesStale_) {
        // Keep path-to-repository mappings: a worktree never changes repository.
        sidebarRepositoriesStale_ = false;
        sidebarRepositoryRoots_.clear();
        for (const QString& root : RepositoryRouter::cachedRecentRepositories()) {
            const QString normalized = conversationRepositoryRoot(root);
            if (!normalized.isEmpty() && isSidebarRepository(normalized) &&
                !sidebarRepositoryRoots_.contains(normalized))
                sidebarRepositoryRoots_.append(normalized);
        }
    }
    // SidebarRepositoryList keeps pin/name order across conversation and team updates.
    QStringList roots;
    for (const QString& root : std::as_const(sidebarRepositoryRoots_)) {
        if (!roots.contains(root))
            roots.append(root);
    }
    if (!activeRepository.isEmpty() && isSidebarRepository(activeRepository) &&
        !roots.contains(activeRepository))
        roots.append(activeRepository);

    // Conversations are stored per worktree. Group them under their repository so the
    // sidebar reads repository > conversation, with the worktree carried by each row.
    QHash<QString, QVariantList> conversationsByRepository;
    // T-92: the open conversation's group is the highlighted one, also after it switched to a
    // worktree of another repository.
    QString selectedRepository;
    bool selectedGroupFound = false;
    if (agentManager_ != nullptr) {
        // The transcript may show a worktree other than the open folder (a deleted worktree
        // opened as history). Selection follows the transcript in every mode, so opening
        // Files or Source Control never highlights another conversation.
        const QString transcriptPath =
            agentWorkspace_ != nullptr ? agentWorkspace_->activePath() : QString{};
        const QString currentWorktree =
            normalizedPath(transcriptPath.isEmpty() ? workspaceRoot_ : transcriptPath);
        QStringList paths = agentManager_->conversationPaths().values();
        // A stable order: moving the open worktree first would reshuffle rows on every click.
        std::sort(paths.begin(), paths.end());
        for (const QString& path : std::as_const(paths)) {
            // T-92: grouped by where it was created (`path`, the daemon's conversation_path,
            // written once), not the worktree it runs in now: switching worktree or repository
            // changes Source Control, Files and the composer's label, never the row's group.
            // Resolved once per worktree: it touches the file system.
            for (const QVariant& value : agentManager_->conversations(path)) {
                QVariantMap conversation = value.toMap();
                const auto fallback =
                    conversation.value(QStringLiteral("sidebarRepositoryFallback")).toString();
                const QString repository =
                    conversation.value(QStringLiteral("sidebarRepositoryKnown")).toBool()
                        ? conversation.value(QStringLiteral("sidebarRepositoryPath")).toString()
                    : fallback.isEmpty() ? sidebarRepositoryForPath(path)
                                         : fallback;
                const bool selected = path == currentWorktree &&
                                      conversation.value(QStringLiteral("active")).toBool();
                // Activating a worktree creates an untitled placeholder. Keep those hidden so
                // review and detached worktrees do not add a row. A conversation the user
                // created, including one that owns a linked worktree, stays listed before the
                // first turn.
                if (!conversation.value(QStringLiteral("started")).toBool() &&
                    !conversation.value(QStringLiteral("running")).toBool() &&
                    conversation.value(QStringLiteral("title")).toString() ==
                        QStringLiteral("New conversation") &&
                    !conversation.value(QStringLiteral("ownsWorktree")).toBool() &&
                    !conversation.value(QStringLiteral("listed")).toBool())
                    continue;
                conversation.insert(QStringLiteral("path"), path);
                conversation.insert(QStringLiteral("selected"), selected);
                if (selected) {
                    selectedRepository = repository;
                    selectedGroupFound = true;
                }
                conversationsByRepository[repository].append(conversation);
                if (!roots.contains(repository))
                    roots.append(repository);
            }
        }
    }

    // Placement is independent of a team's mutable execution repository (ADR 0123).
    const QHash<QString, QList<QJsonObject>> sidebarTeams =
        teams_ != nullptr && teams_->hasTeams() ? teams_->sidebarTeamsByRepository()
                                                : QHash<QString, QList<QJsonObject>>{};
    if (teams_ != nullptr)
        for (const auto& team : teams_->teams()) {
            const auto root = team.value(QStringLiteral("sidebar_repository"))
                                  .toString(team.value(QStringLiteral("repository")).toString());
            if (!roots.contains(root))
                roots.append(root);
        }
    // Members stay under their team, whichever execution worktree a task gives them.
    QHash<QString, QVector<SidebarTeam>> teamsByRepository;
    for (const QString& root : std::as_const(roots)) {
        auto folders = takeSidebarTeams(root, sidebarTeams, conversationsByRepository);
        for (const auto& team : folders)
            for (const auto& member : team.members)
                if (member.toMap().value(QStringLiteral("selected")).toBool()) {
                    selectedRepository = root;
                    selectedGroupFound = true;
                }
        teamsByRepository.insert(root, folders);
    }
    QVector<SidebarRepository> repositories;
    repositories.reserve(roots.size());
    for (const QString& root : std::as_const(roots))
        repositories.append(
            {.path = root,
             .name = root.isEmpty() ? tr("Other conversations") : QFileInfo(root).fileName(),
             .active = root == (selectedGroupFound ? selectedRepository : activeRepository),
             .conversations = conversationsByRepository.value(root),
             .teams = teamsByRepository.value(root),
             .pinnable = !root.isEmpty()});
    activityBar_->setRepositories(repositories);
    updateTeamRoles();
    if (trace) {
        qsizetype conversations = 0;
        for (const SidebarRepository& repository : std::as_const(repositories))
            conversations += repository.conversations.size();
        std::fprintf(
            stderr, "KETPLUS_SIDEBAR_REFRESH ms=%.2f repositories=%lld conversations=%lld\n",
            double(timer.nsecsElapsed()) / 1e6, static_cast<long long>(repositories.size()),
            static_cast<long long>(conversations));
    }
}

void MainWindow::openSidebarConversation(const QString& worktreePath,
                                         const QString& conversationId) {
    if (selectedMachineId_ != QStringLiteral("local")) {
        // A remote row names a host path or a Relay id; it is never resolved on This Mac.
        setMachineWorkspaceVisible(true);
        if (machines_->shown() && !conversationId.isEmpty())
            machines_->existingView()->openConversation(conversationId);
        return;
    }
    const QString path = normalizedPath(worktreePath);
    if (path.isEmpty() || conversationId.isEmpty())
        return;
    // The sidebar spans repositories, so navigation stays in this window.
    setAgentWorkspaceVisible(true);
    if (!QFileInfo(path).isDir()) {
        // A deleted worktree keeps its history: open it read-only instead of as a folder. Its
        // repository still opens, so Files, Source Control and Atlas show the repository the
        // sidebar selected rather than whichever was open before.
        const QString repository = sidebarRepositoryForPath(path);
        if (!repository.isEmpty() && repository != activeSidebarRepository()) {
            const bool suppressRestore = suppressWorkspaceRestore_;
            suppressWorkspaceRestore_ = true;
            openFolder(repository);
            suppressWorkspaceRestore_ = suppressRestore;
            setAgentWorkspaceVisible(true);
        }
        if (agentWorkspace_ != nullptr)
            agentWorkspace_->setActivePath(path);
        if (agentManager_ != nullptr) {
            agentManager_->activate(path, true, conversationId);
            agentManager_->selectConversation(path, conversationId);
        }
        statusBar()->showMessage(QStringLiteral("Worktree missing. The agent continues from the "
                                                "repository's main worktree."),
                                 7000);
        scheduleSidebarRefresh();
        return;
    }
    if (path != normalizedPath(workspaceRoot_)) {
        // Opening another repository restores its saved workspace state, which can switch to
        // Explorer or Source Control, or to a different worktree and conversation. The sidebar
        // asked for this conversation, so skip that restore.
        const bool suppressRestore = suppressWorkspaceRestore_;
        suppressWorkspaceRestore_ = true;
        // Activating the worktree opens this conversation, not the one it had open.
        navigatingConversationId_ = conversationId;
        if (sidebarRepositoryForPath(path) == activeSidebarRepository())
            switchToWorktree(path);
        else
            openFolder(path);
        navigatingConversationId_.clear();
        suppressWorkspaceRestore_ = suppressRestore;
        // Whatever navigation showed, a conversation opens in the Agent Workspace.
        setAgentWorkspaceVisible(true);
    }
    if (agentManager_ != nullptr)
        agentManager_->selectConversation(path, conversationId);
}

} // namespace ketplus
