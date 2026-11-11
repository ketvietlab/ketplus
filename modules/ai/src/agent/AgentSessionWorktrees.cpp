#include "agent/AgentSessionManager.h"
#include "agent/AgentSessionState.h"
#include "agent/CodexAppServer.h"
#include "agent/ConversationWorktreeStore.h"

#include "git/GitParser.h"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include <memory>

namespace ketplus {
namespace {
QString gitExecutable() {
    QString found = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (found.isEmpty())
        found = QStandardPaths::findExecutable(
            QStringLiteral("git"),
            {QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin")});
    return found;
}

// Newest messages first, and only as far back as a turn can plausibly reach.
constexpr int kTurnMessageScanLimit = 4000;

bool mentions(const QVariantMap& message, const QString& path, const QString& name) {
    const auto contains = [&](const QString& text) {
        return !text.isEmpty() && (text.contains(path) || text.contains(name));
    };
    for (const char* key : {"command", "cwd", "path", "body", "summary", "target"}) {
        if (contains(message.value(QString::fromLatin1(key)).toString()))
            return true;
    }
    for (const QVariant& change : message.value(QStringLiteral("fileChanges")).toList()) {
        if (contains(change.toMap().value(QStringLiteral("path")).toString()))
            return true;
    }
    return false;
}
} // namespace

void AgentSessionManager::executionWorktreeCommand(
    const QString& path, const QString& type, const QJsonObject& payload,
    std::function<void(const QJsonObject&, const QString&)> callback) {
    const auto session = ensureSession(path);
    if (!session || session->threadId.isEmpty()) {
        callback({}, "Start a conversation before choosing its execution worktree.");
        return;
    }
    server_->executionWorktreeCommand(session->threadId, type, payload, std::move(callback));
}

// `git worktree list` runs off the GUI thread's event loop: the turn notifications that
// trigger it arrive on the GUI thread, and a slow or locked repository must not stall chat.
void AgentSessionManager::listRepositoryWorktrees(
    const QString& cwd, std::function<void(bool, const QStringList&)> done) {
    const QString root = normalizedPath(cwd);
    const QString git = gitExecutable();
    if (root.isEmpty() || !QFileInfo(root).isDir() || git.isEmpty()) {
        done(false, {});
        return;
    }
    auto* process = new QProcess(this);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    process->setProcessEnvironment(environment);
    process->setWorkingDirectory(root);
    auto finish = std::make_shared<std::function<void(bool, const QStringList&)>>(std::move(done));
    const auto deliver = [process, finish](const bool ok, const QStringList& paths) {
        if (!*finish)
            return;
        const auto callback = std::move(*finish);
        *finish = nullptr;
        process->deleteLater();
        callback(ok, paths);
    };
    connect(process, &QProcess::finished, this,
            [process, root, deliver](const int exitCode, const QProcess::ExitStatus status) {
                if (status != QProcess::NormalExit || exitCode != 0) {
                    deliver(false, {});
                    return;
                }
                QStringList paths;
                for (const GitWorktree& worktree :
                     git_parser::parseWorktrees(process->readAllStandardOutput(), root)) {
                    if (worktree.bare || worktree.prunable || worktree.path.isEmpty())
                        continue;
                    paths.append(normalizedPath(worktree.path));
                }
                deliver(true, paths);
            });
    connect(process, &QProcess::errorOccurred, this, [deliver](const QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart || error == QProcess::Crashed)
            deliver(false, {});
    });
    QTimer::singleShot(5000, process, [process] { process->kill(); });
    process->start(git, {QStringLiteral("worktree"), QStringLiteral("list"),
                         QStringLiteral("--porcelain"), QStringLiteral("-z")});
}

void AgentSessionManager::snapshotTurnWorktrees(const SessionPtr& session) {
    if (!session || !worktreeStore_)
        return;
    const QString turnId = session->turnId;
    const quint64 generation = ++session->turnWorktreeGeneration;
    session->turnWorktreeSnapshot.clear();
    session->turnWorktreeSnapshotValid = false;
    session->turnWorktreeTurnId = turnId;
    std::weak_ptr<Session> weak = session;
    listRepositoryWorktrees(session->path,
                            [weak, generation](const bool ok, const QStringList& paths) {
                                const SessionPtr current = weak.lock();
                                if (!current || current->turnWorktreeGeneration != generation)
                                    return;
                                // A failed listing skips capture for this turn rather than
                                // attributing every checkout.
                                current->turnWorktreeSnapshot = paths;
                                current->turnWorktreeSnapshotValid = ok && !paths.isEmpty();
                            });
}

void AgentSessionManager::captureTurnWorktrees(const SessionPtr& session) {
    if (!session || !worktreeStore_)
        return;
    const quint64 generation = ++session->turnWorktreeGeneration;
    if (!session->turnWorktreeSnapshotValid)
        return;
    const QStringList before = session->turnWorktreeSnapshot;
    const QString turnId = session->turnWorktreeTurnId;
    session->turnWorktreeSnapshot.clear();
    session->turnWorktreeSnapshotValid = false;
    std::weak_ptr<Session> weak = session;
    listRepositoryWorktrees(session->path, [this, weak, generation, before,
                                            turnId](const bool ok, const QStringList& after) {
        const SessionPtr current = weak.lock();
        if (!ok || !current || current->turnWorktreeGeneration != generation)
            return;
        const QStringList created =
            ConversationWorktreeStore::createdDuringTurn(before, after, mainCheckoutPath(after));
        bool changed = false;
        for (const QString& path : created) {
            // Git records no creator. Claim a checkout only when this turn's own transcript
            // names it, so a worktree made meanwhile by the user or another conversation is
            // never offered for deletion with this one.
            if (!turnMentionsPath(current, turnId, path) ||
                worktreeStore_->containsPath(path, current->id))
                continue;
            worktreeStore_->save(current->id, path,
                                 QString::fromLatin1(ConversationWorktreeStore::kRoleAgent));
            changed = true;
        }
        if (changed) {
            emit ownedWorktreesChanged(current->path);
            emit conversationsChanged(current->path, conversations(current->path));
        }
    });
}

bool AgentSessionManager::turnMentionsPath(const SessionPtr& session, const QString& turnId,
                                           const QString& path) {
    if (!session || turnId.isEmpty() || path.isEmpty())
        return false;
    const QString name = QFileInfo(path).fileName();
    if (name.isEmpty())
        return false;
    const int last = static_cast<int>(session->messages.size()) - 1;
    for (int index = last; index >= 0 && last - index < kTurnMessageScanLimit; --index) {
        const QVariantMap& message = session->messages.at(index);
        if (message.value(QStringLiteral("turnId")).toString() == turnId &&
            mentions(message, path, name))
            return true;
    }
    return false;
}

QString AgentSessionManager::mainCheckoutPath(const QStringList& worktrees) {
    for (const QString& path : worktrees) {
        if (QFileInfo(QDir(path).filePath(QStringLiteral(".git"))).isDir())
            return path;
    }
    return worktrees.isEmpty() ? QString{} : worktrees.constFirst();
}

QString AgentSessionManager::recentWorkingCheckout(const QString& worktreePath) const {
    const QString own = normalizedPath(worktreePath);
    const SessionPtr session = sessions_.value(own);
    if (!session)
        return {};
    const auto checkoutOf = [&own](const QString& location) -> QString {
        if (location.isEmpty() || !QDir::isAbsolutePath(location))
            return {};
        QDir folder(location);
        if (!QFileInfo(location).isDir() && !folder.cdUp())
            return {};
        do {
            if (!folder.exists())
                continue;
            if (QFileInfo::exists(folder.filePath(QStringLiteral(".git")))) {
                const QString checkout = normalizedPath(folder.absolutePath());
                return checkout == own ? QString{} : checkout;
            }
        } while (folder.cdUp());
        return {};
    };
    const int last = static_cast<int>(session->messages.size()) - 1;
    for (int index = last; index >= 0 && last - index < kTurnMessageScanLimit; --index) {
        const QVariantMap& message = session->messages.at(index);
        for (const QVariant& change : message.value(QStringLiteral("fileChanges")).toList()) {
            const QString checkout =
                checkoutOf(change.toMap().value(QStringLiteral("path")).toString());
            if (!checkout.isEmpty())
                return checkout;
        }
        const QString checkout = checkoutOf(message.value(QStringLiteral("cwd")).toString());
        if (!checkout.isEmpty())
            return checkout;
    }
    return {};
}

QVariantList AgentSessionManager::ownedWorktrees(const QString& worktreePath) const {
    QVariantList result;
    if (!worktreeStore_)
        return result;
    const SessionPtr session = sessions_.value(normalizedPath(worktreePath));
    if (!session)
        return result;
    for (const auto& record : worktreeStore_->list(session->id)) {
        if (record.hidden)
            continue;
        result.append(
            QVariantMap{{QStringLiteral("path"), record.path},
                        {QStringLiteral("role"), record.role},
                        {QStringLiteral("primary"),
                         record.role == QLatin1String(ConversationWorktreeStore::kRolePrimary)}});
    }
    return result;
}

QStringList
AgentSessionManager::conversationOwnedWorktreePaths(const QString& worktreePath,
                                                    const QString& conversationId) const {
    Q_UNUSED(worktreePath);
    if (!worktreeStore_ || conversationId.isEmpty())
        return {};
    return worktreeStore_->allPaths(conversationId);
}

void AgentSessionManager::claimConversationWorktree(const QString& worktreePath,
                                                    const QString& conversationId,
                                                    const QString& ownedPath) {
    if (!worktreeStore_ || conversationId.isEmpty() || ownedPath.isEmpty() ||
        worktreeStore_->containsPath(ownedPath, conversationId))
        return;
    worktreeStore_->save(conversationId, ownedPath,
                         QString::fromLatin1(ConversationWorktreeStore::kRoleAgent));
    const QString path = normalizedPath(worktreePath);
    emit ownedWorktreesChanged(path);
    emit conversationsChanged(path, conversations(path));
}

void AgentSessionManager::hideOwnedWorktree(const QString& worktreePath,
                                            const QString& conversationId,
                                            const QString& ownedPath) {
    if (!worktreeStore_ || conversationId.isEmpty())
        return;
    worktreeStore_->hide(conversationId, ownedPath);
    emit ownedWorktreesChanged(normalizedPath(worktreePath));
}

} // namespace ketplus
