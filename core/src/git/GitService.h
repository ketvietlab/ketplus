#pragma once

#include "git/GitTypes.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace ketplus {

class GitService final : public QObject {
    Q_OBJECT

  public:
    explicit GitService(QObject* parent = nullptr);
    ~GitService() override;

    [[nodiscard]] bool isAvailable() const noexcept;
    [[nodiscard]] const GitSnapshot& snapshot() const noexcept;
    [[nodiscard]] const QVector<GitWorktree>& worktrees() const noexcept;

    void setWorkspacePath(const QString& path);
    void refresh();
    void scheduleRefresh(int delayMilliseconds = 400);
    void stageFile(const QString& filePath);
    void unstageFile(const QString& filePath);
    void commitStaged(const QString& message);
    [[nodiscard]] bool mutationRunning() const { return mutationRunning_; }
    void requestDiff(const QString& filePath, GitDiffMode mode = GitDiffMode::Combined);

  signals:
    void snapshotChanged(const ketplus::GitSnapshot& snapshot);
    void worktreesChanged(const QVector<ketplus::GitWorktree>& worktrees);
    void diffReady(const QString& filePath, ketplus::GitDiffMode mode, const QString& diff,
                   const QString& error);
    void mutationRunningChanged(bool running);
    void mutationFinished(const QString& operation, bool ok, const QString& error);
    void errorOccurred(const QString& message);

  private:
    using CommandCallback = std::function<void(int exitCode, const QByteArray& output,
                                               const QByteArray& error, bool completedNormally)>;

    void runGit(const QStringList& arguments, const QString& workingDirectory,
                CommandCallback callback, int timeoutMilliseconds = 10000);
    [[nodiscard]] bool isUntracked(const QString& relativePath) const;
    void watchGitState();
    static QString canonicalEntryPath(const QString& path);
    void mutateFile(const QString& filePath, bool stage);
    void runMutation(const QString& operation, const QStringList& arguments);
    bool mutationRunning_{false};

    QString gitExecutable_;
    QString workspacePath_;
    QString repositoryRoot_;
    GitSnapshot snapshot_;
    QVector<GitWorktree> worktrees_;
    quint64 workspaceGeneration_{0};
    quint64 refreshGeneration_{0};
    // Only the newest diff request is answered; an older reply would replace it.
    quint64 diffGeneration_{0};
    // Git commands run outside KetPlus (terminal, agents) change the index, HEAD and reflog
    // in the repository's git dir; watching it keeps Source Control current.
    QString gitDirectory_;
    QFileSystemWatcher gitStateWatcher_;
    QTimer* refreshTimer_{nullptr};
};

} // namespace ketplus
