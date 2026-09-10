#pragma once

#include "git/GitTypes.h"

#include <QObject>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace ketplus {

class GitService final : public QObject {
    Q_OBJECT

  public:
    explicit GitService(QObject* parent = nullptr);

    [[nodiscard]] bool isAvailable() const noexcept;
    [[nodiscard]] const GitSnapshot& snapshot() const noexcept;
    [[nodiscard]] const QVector<GitWorktree>& worktrees() const noexcept;

    void setWorkspacePath(const QString& path);
    void refresh();
    void scheduleRefresh(int delayMilliseconds = 400);
    void requestDiff(const QString& filePath, GitDiffMode mode = GitDiffMode::Combined);

  signals:
    void snapshotChanged(const ketplus::GitSnapshot& snapshot);
    void worktreesChanged(const QVector<ketplus::GitWorktree>& worktrees);
    void diffReady(const QString& filePath, ketplus::GitDiffMode mode, const QString& diff,
                   const QString& error);
    void errorOccurred(const QString& message);

  private:
    using CommandCallback =
        std::function<void(int exitCode, const QByteArray& output, const QByteArray& error,
                           bool completedNormally)>;

    void runGit(const QStringList& arguments, const QString& workingDirectory,
                CommandCallback callback, int timeoutMilliseconds = 10000);
    [[nodiscard]] bool isUntracked(const QString& relativePath) const;

    QString gitExecutable_;
    QString workspacePath_;
    QString repositoryRoot_;
    GitSnapshot snapshot_;
    QVector<GitWorktree> worktrees_;
    quint64 workspaceGeneration_{0};
    quint64 refreshGeneration_{0};
    QTimer* refreshTimer_{nullptr};
};

} // namespace ketplus
