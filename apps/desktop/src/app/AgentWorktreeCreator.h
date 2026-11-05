#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>

namespace ketplus {

class AgentWorktreeCreator final : public QObject {
    Q_OBJECT

  public:
    explicit AgentWorktreeCreator(QObject* parent = nullptr);

    [[nodiscard]] static QString directoryNameForBranch(const QString& branchName);
    [[nodiscard]] static QString worktreePath(const QString& primaryWorktreePath,
                                              const QString& branchName);
    [[nodiscard]] bool isRunning() const noexcept;

    void create(const QString& repositoryPath, const QString& primaryWorktreePath,
                const QString& branchName, const QString& baseBranch = QStringLiteral("HEAD"));
    void remove(const QString& repositoryPath, const QString& worktreePath, bool force = false);
    void prune(const QString& repositoryPath);

  signals:
    void created(const QString& path, const QString& branchName);
    void failed(const QString& error);
    void removed(const QString& path);
    void pruned();

  private:
    void appendOutput();
    void finish(bool succeeded, const QString& fallbackError = {});

    QProcess process_;
    QTimer timeout_;
    QByteArray output_;
    QString targetPath_;
    QString branchName_;
    bool active_{false};
    bool timedOut_{false};
    bool removing_{false};
    bool pruning_{false};
};

} // namespace ketplus
