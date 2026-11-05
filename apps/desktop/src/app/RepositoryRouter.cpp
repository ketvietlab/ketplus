#include "app/RepositoryRouter.h"
#include "app/RepositoryMetadataStore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QWidget>

namespace ketplus {
namespace {
struct RepositoryLocation {
    QString identity;
    QString worktree;
    bool verified{false};
    bool unavailable{false};
};

RepositoryLocation locateRepository(const QString& path) {
    const auto canonical = RepositoryRouter::canonicalPath(path);
    QProcess git;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove("GIT_DIR");
    environment.remove("GIT_WORK_TREE");
    environment.remove("GIT_COMMON_DIR");
    git.setProcessEnvironment(environment);
    git.start("git", {"-C", canonical, "rev-parse", "--path-format=absolute", "--git-common-dir", "--show-toplevel"});
    if (!git.waitForFinished(1500)) {
        git.kill();
        git.waitForFinished();
        // A slow Git is not "no repository": keep the last verified identity, so linked
        // worktrees still route to one window and endpoint.
        if (const auto cached = RepositoryMetadataStore().lookup(canonical)) {
            // `canonical` can be a folder inside the checkout; route to the checkout itself.
            const QString checkout = RepositoryMetadataStore::checkoutRoot(canonical);
            return {cached->identity, checkout.isEmpty() ? canonical : checkout};
        }
        return {canonical, canonical};
    }
    const auto lines = QString::fromUtf8(git.readAllStandardOutput()).trimmed().split('\n');
    if (git.exitStatus() != QProcess::NormalExit || git.exitCode() != 0 || lines.size() != 2)
        return {canonical, canonical, false,
                git.exitStatus() == QProcess::NormalExit && git.exitCode() != 0};
    return {RepositoryRouter::canonicalPath(lines[0]), RepositoryRouter::canonicalPath(lines[1]),
            true};
}

QString endpointForIdentity(const QString& identity) {
    const auto key = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + ":" + identity;
    return "ketplus-repo-v2-" + QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
}

// A routing request is one short JSON line; anything longer is not a KetPlus client.
constexpr qint64 maximumRequestBytes = 16 * 1024;
// Longer than two Git lookups, shorter than the client's 5 s wait.
constexpr int requestLifetimeMs = 4500;

void raiseWindow(QWidget* window) {
    if (window->isMinimized()) window->showNormal();
    window->show();
    window->raise();
    window->activateWindow();
}
}

RepositoryRouter::RepositoryRouter(QObject* parent) : QObject(parent) {
    lookupPool_.setMaxThreadCount(2);
}

QString RepositoryRouter::canonicalPath(const QString& path) {
    const QFileInfo info(path);
    return QDir::cleanPath(info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath());
}

QString RepositoryRouter::endpointName(const QString& path) {
    return endpointForIdentity(locateRepository(path).identity);
}

QStringList RepositoryRouter::recentRepositories() {
    QSettings settings;
    settings.sync();
    QStringList result;
    const auto paths = settings.value("spotlight/repositories").toStringList()
                       + settings.value("workspace/recentFolders").toStringList();
    for (const auto& path : paths) {
        if (path.isEmpty()) continue;
        const auto normalized = repositoryRoot(path);
        if (!result.contains(normalized)) result.append(normalized);
    }
    return result;
}

QString RepositoryRouter::repositoryRoot(const QString& path) {
    if (path.isEmpty())
        return {};
    const auto location = locateRepository(path);
    RepositoryMetadataStore store;
    const auto normalized = canonicalPath(path);
    if (!location.verified) {
        if (location.unavailable || !QFileInfo(path).isDir())
            store.remove(normalized);
        else if (const auto cached = store.lookup(normalized))
            return cached->root;
        return location.worktree;
    }
    const auto root = QFileInfo(location.identity).fileName() == QStringLiteral(".git")
                          ? QFileInfo(location.identity).absolutePath()
                          : location.worktree;
    store.save({normalized, location.identity, root});
    return root;
}

QString RepositoryRouter::cachedRepositoryRoot(const QString& path) {
    if (path.isEmpty())
        return {};
    const auto metadata = RepositoryMetadataStore().lookup(canonicalPath(path));
    return metadata ? metadata->root : QString{};
}

QStringList RepositoryRouter::cachedRecentRepositories() {
    QSettings settings;
    const auto paths = settings.value("spotlight/repositories").toStringList() +
                       settings.value("workspace/recentFolders").toStringList();
    QStringList canonical;
    canonical.reserve(paths.size());
    for (const auto& path : paths) {
        if (!path.isEmpty())
            canonical.append(canonicalPath(path));
    }
    const auto metadata = RepositoryMetadataStore().lookupAll(canonical);
    QStringList result;
    for (const auto& path : std::as_const(canonical)) {
        const auto found = metadata.constFind(path);
        if (found != metadata.cend() && !found->root.isEmpty() && !result.contains(found->root))
            result.append(found->root);
    }
    return result;
}

void RepositoryRouter::remember(const QString& path) {
    QSettings settings;
    auto paths = settings.value("spotlight/repositories").toStringList();
    paths.removeAll(path);
    paths.prepend(path);
    settings.setValue("spotlight/repositories", paths);
    settings.sync();
}

void RepositoryRouter::registerWindow(const QString& path, QWidget* window) {
    // Supersedes a registration still resolving on the pool.
    ++registrationGenerations_[window];
    const auto location = locateRepository(path);
    registerLocation(location.identity, location.worktree, window);
}

// Opening a folder must not wait for Git, so the repository is located on the pool. The window
// registers when the answer lands, unless it opened another folder in the meantime.
void RepositoryRouter::registerWindowLater(const QString& path, QWidget* window) {
    if (!registrationGenerations_.contains(window))
        connect(window, &QObject::destroyed, this,
                [this, window] { registrationGenerations_.remove(window); });
    const quint64 generation = ++registrationGenerations_[window];
    lookupPool_.start([router = QPointer<RepositoryRouter>(this),
                       target = QPointer<QWidget>(window), generation, path] {
        const auto location = locateRepository(path);
        QMetaObject::invokeMethod(
            qApp,
            [router, target, generation, location] {
                if (!router || !target ||
                    router->registrationGenerations_.value(target) != generation)
                    return;
                router->registerLocation(location.identity, location.worktree, target);
            },
            Qt::QueuedConnection);
    });
}

void RepositoryRouter::registerLocation(const QString& normalized, const QString& worktree,
                                        QWidget* window) {
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (it.value().isNull() || (it.value() == window && it.key() != normalized)) {
            delete servers_.take(it.key());
            windowPaths_.remove(it.key());
            it = windows_.erase(it);
        } else ++it;
    }
    remember(worktree);
    if (windows_.value(normalized) && windows_.value(normalized) != window) return;
    windows_.insert(normalized, window);
    windowPaths_.insert(normalized, worktree);
    if (servers_.contains(normalized)) return;
    auto* server = new QLocalServer(this);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    const auto name = endpointForIdentity(normalized);
    if (!server->listen(name)) {
        QLocalSocket probe;
        probe.connectToServer(name);
        if (probe.waitForConnected(50)) {
            delete server;
            return;
        }
        QLocalServer::removeServer(name);
        if (!server->listen(name)) { delete server; return; }
    }
    servers_.insert(normalized, server);
    connect(window, &QObject::destroyed, server, [this, normalized, server] {
        windows_.remove(normalized);
        windowPaths_.remove(normalized);
        servers_.remove(normalized);
        server->deleteLater();
    });
    connect(server, &QLocalServer::newConnection, this, [this, server, normalized] {
        while (auto* socket = server->nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(requestLifetimeMs, socket, &QObject::deleteLater);
            // Bounds what Qt buffers for this client, including while the lookup runs.
            socket->setReadBufferSize(maximumRequestBytes + 1);
            connect(socket, &QLocalSocket::readyRead, socket, [this, socket, normalized] {
                if (!socket->canReadLine()) {
                    if (socket->bytesAvailable() > maximumRequestBytes)
                        socket->abort();
                    return;
                }
                const QByteArray line = socket->readLine(maximumRequestBytes + 2);
                if (line.size() > maximumRequestBytes || !line.endsWith('\n')) {
                    socket->abort();
                    return;
                }
                // One request per connection: anything after it ends the connection.
                disconnect(socket, &QLocalSocket::readyRead, socket, nullptr);
                connect(socket, &QLocalSocket::readyRead, socket, [socket] { socket->abort(); });
                if (socket->bytesAvailable() > 0) {
                    socket->abort();
                    return;
                }
                const auto request = QJsonDocument::fromJson(line).object();
                const QString action = request.value("action").toString();
                const bool opensFile = action == QStringLiteral("openFile") ||
                                       action == QStringLiteral("openFileDiff");
                // Locating the repository runs Git, which must not block this window.
                lookupPool_.start([router = QPointer<RepositoryRouter>(this),
                                   client = QPointer<QLocalSocket>(socket), normalized, request,
                                   action, opensFile] {
                    RepositoryLocation location =
                        locateRepository(request.value("path").toString());
                    QString filePath;
                    if (opensFile) {
                        filePath = canonicalPath(request.value("filePath").toString());
                        if (QFileInfo(filePath).isFile())
                            location = locateRepository(QFileInfo(filePath).absolutePath());
                        else
                            filePath.clear();
                    }
                    QMetaObject::invokeMethod(
                        qApp,
                        [router, client, normalized, action, opensFile, location, filePath] {
                            if (router && client)
                                router->answerRequest(client, normalized, action, opensFile,
                                                      location.identity, location.worktree,
                                                      filePath);
                        },
                        Qt::QueuedConnection);
                });
            });
        }
    });
}

void RepositoryRouter::answerRequest(QLocalSocket* socket, const QString& normalized,
                                     const QString& action, const bool opensFile,
                                     const QString& identity, const QString& worktree,
                                     const QString& filePath) {
    const RepositoryLocation location{identity, worktree};
    if (opensFile && filePath.isEmpty()) {
        socket->disconnectFromServer();
        return;
    }
    if ((action == QStringLiteral("activate") || opensFile) &&
        QFileInfo(location.worktree).isDir() && location.identity == normalized &&
        windows_.value(normalized)) {
        auto* window = windows_.value(normalized).data();
        if (windowPaths_.value(normalized) != location.worktree)
            emit worktreeRequested(window, location.worktree);
        if (!filePath.isEmpty()) {
            if (action == QStringLiteral("openFileDiff"))
                emit fileDiffRequested(window, filePath);
            else
                emit fileRequested(window, filePath);
        }
        raiseWindow(window);
        remember(location.worktree);
        socket->write("ok\n");
        socket->flush();
    }
    socket->disconnectFromServer();
}

void RepositoryRouter::requestRemote(const QString& identity, const QString& worktreePath,
                                     const QString& filePath, const bool openDiff) {
    auto* socket = new QLocalSocket(this);
    auto* timeout = new QTimer(socket);
    timeout->setSingleShot(true);
    connect(socket, &QLocalSocket::connected, socket, [socket, worktreePath, filePath, openDiff] {
        QJsonObject request{{"action", filePath.isEmpty() ? "activate"
                                       : openDiff         ? "openFileDiff"
                                                          : "openFile"},
                            {"path", worktreePath}};
        if (!filePath.isEmpty())
            request.insert(QStringLiteral("filePath"), filePath);
        socket->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    });
    connect(socket, &QLocalSocket::readyRead, socket, [socket, timeout] {
        if (socket->canReadLine() && socket->readLine() == "ok\n") {
            timeout->stop();
            socket->disconnect();
            socket->deleteLater();
        }
    });
    connect(socket, &QLocalSocket::errorOccurred, this,
            [this, socket, timeout, worktreePath, filePath, openDiff](auto error) {
                timeout->stop();
                socket->disconnect();
                socket->deleteLater();
                if (error == QLocalSocket::ServerNotFoundError ||
                    error == QLocalSocket::ConnectionRefusedError) {
                    if (filePath.isEmpty())
                        emit openRequested(worktreePath);
                    else if (openDiff)
                        emit openFileDiffRequested(worktreePath, filePath);
                    else
                        emit openFileRequested(worktreePath, filePath);
                } else {
                    emit activationFailed(
                        tr("Unable to switch to repository: %1").arg(worktreePath));
                }
            });
    connect(timeout, &QTimer::timeout, this, [this, socket, worktreePath] {
        socket->disconnect();
        socket->deleteLater();
        emit activationFailed(tr("Repository window did not respond: %1").arg(worktreePath));
    });
    timeout->start(5000);
    socket->connectToServer(endpointForIdentity(identity));
}

void RepositoryRouter::activate(const QString& path) {
    const auto location = locateRepository(path);
    const auto normalized = location.worktree;
    if (!QFileInfo(normalized).isDir()) {
        emit activationFailed(tr("Repository folder is unavailable: %1").arg(path));
        return;
    }
    if (windows_.value(location.identity)) {
        auto* window = windows_.value(location.identity).data();
        if (windowPaths_.value(location.identity) != normalized)
            emit worktreeRequested(window, normalized);
        raiseWindow(window);
        remember(normalized);
        return;
    }
    requestRemote(location.identity, normalized);
}

void RepositoryRouter::openFile(const QString& filePath) {
    const QString normalizedFile = canonicalPath(filePath);
    if (!QFileInfo(normalizedFile).isFile()) {
        emit activationFailed(tr("File is unavailable: %1").arg(filePath));
        return;
    }

    const RepositoryLocation location = locateRepository(QFileInfo(normalizedFile).absolutePath());
    if (windows_.value(location.identity)) {
        auto* window = windows_.value(location.identity).data();
        if (windowPaths_.value(location.identity) != location.worktree)
            emit worktreeRequested(window, location.worktree);
        emit fileRequested(window, normalizedFile);
        raiseWindow(window);
        remember(location.worktree);
        return;
    }
    requestRemote(location.identity, location.worktree, normalizedFile);
}

void RepositoryRouter::openFileDiff(const QString& filePath) {
    const QString normalizedFile = canonicalPath(filePath);
    if (!QFileInfo(normalizedFile).isFile()) {
        emit activationFailed(tr("File is unavailable: %1").arg(filePath));
        return;
    }

    const RepositoryLocation location = locateRepository(QFileInfo(normalizedFile).absolutePath());
    if (windows_.value(location.identity)) {
        auto* window = windows_.value(location.identity).data();
        if (windowPaths_.value(location.identity) != location.worktree)
            emit worktreeRequested(window, location.worktree);
        emit fileDiffRequested(window, normalizedFile);
        raiseWindow(window);
        remember(location.worktree);
        return;
    }
    requestRemote(location.identity, location.worktree, normalizedFile, true);
}

} // namespace ketplus
