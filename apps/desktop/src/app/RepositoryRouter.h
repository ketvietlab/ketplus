#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QThreadPool>

class QWidget;
class QLocalServer;

namespace ketplus {

class RepositoryRouter final : public QObject {
    Q_OBJECT
  public:
    explicit RepositoryRouter(QObject* parent = nullptr);
    void registerWindow(const QString& path, QWidget* window);
    void registerWindowLater(const QString& path, QWidget* window);
    void activate(const QString& path);
    void openFile(const QString& filePath);
    void openFileDiff(const QString& filePath);
    static QStringList recentRepositories();
    static QStringList cachedRecentRepositories();
    static QString cachedRepositoryRoot(const QString& path);
    static QString repositoryRoot(const QString& path);
    static QString canonicalPath(const QString& path);
    static QString endpointName(const QString& path);

  signals:
    void openRequested(const QString& path);
    void openFileRequested(const QString& repositoryPath, const QString& filePath);
    void openFileDiffRequested(const QString& repositoryPath, const QString& filePath);
    void worktreeRequested(QWidget* window, const QString& path);
    void fileRequested(QWidget* window, const QString& filePath);
    void fileDiffRequested(QWidget* window, const QString& filePath);
    void activationFailed(const QString& message);

  private:
    static void remember(const QString& path);
    void registerLocation(const QString& normalized, const QString& worktree, QWidget* window);
    void requestRemote(const QString& identity, const QString& worktreePath,
                       const QString& filePath = {}, bool openDiff = false);
    QHash<QString, QPointer<QWidget>> windows_;
    QHash<QString, QString> windowPaths_;
    QHash<QString, QLocalServer*> servers_;
    QHash<QWidget*, quint64> registrationGenerations_;
    // Destroyed first, so it waits for Git lookups before the router state goes away.
    QThreadPool lookupPool_;
};

} // namespace ketplus
