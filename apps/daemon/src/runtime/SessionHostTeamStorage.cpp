// ADR 0114 §10: the team page's disk warning and the removal of a done task's worktree, in the
// session runtime. Measuring and git run off the event loop; the requests are served only to
// the desktop (handleCommand refuses team.* from any other client).
#include "runtime/SessionHost.h"
#include "runtime/TeamWorktreeRemoval.h"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QStorageInfo>

namespace ketplus::daemon {
namespace {

QJsonObject refusedAnswer(const teams::TeamError& error) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), error.code},
            {QStringLiteral("error_message"), error.message}};
}

} // namespace

void SessionHost::askStorage(const QString& volumePath, const QStringList& worktrees,
                             teams::StorageDone done) {
    QThreadPool::globalInstance()->start([guard = QPointer<SessionHost>(this), volumePath,
                                          worktrees, done = std::move(done)]() mutable {
        teams::StorageFacts facts;
        QStorageInfo volume(volumePath);
        if (volume.isValid() && volume.isReady())
            facts.freeBytes = volume.bytesAvailable();
        for (const QString& worktree : worktrees)
            if (QFileInfo(worktree).isDir())
                facts.sizes.insert(worktree, team_git::sizeOnDisk(worktree));
        QMetaObject::invokeMethod(
            guard, [done = std::move(done), facts = std::move(facts)] { done(facts); },
            Qt::QueuedConnection);
    });
}

// In the order of the team's other git questions, so no command reads a half-removed worktree.
void SessionHost::askRemoveWorktree(const QString& repository, const QString& worktree,
                                    teams::RemovalDone done) {
    teamGit_.start([guard = QPointer<SessionHost>(this), repository, worktree,
                    done = std::move(done)]() mutable {
        QString error = team_git::removeWorktree(repository, worktree);
        QMetaObject::invokeMethod(
            guard, [done = std::move(done), error = std::move(error)] { done(error); },
            Qt::QueuedConnection);
    });
}

QString SessionHost::conversationRunningIn(const QString& path) const {
    const QString folder = QDir::cleanPath(path);
    for (const Session& session : sessions_) {
        const QString where = QDir::cleanPath(session.stored.worktreePath);
        if (where != folder && !where.startsWith(folder + QLatin1Char('/')))
            continue;
        if (!agentConversationOf(session).idle)
            return session.stored.title.isEmpty() ? session.stored.id : session.stored.title;
    }
    return {};
}

QStringList SessionHost::conversationsIn(const QString& path) const {
    const QString folder = QDir::cleanPath(path);
    QStringList found;
    for (const Session& session : sessions_) {
        const QString where = QDir::cleanPath(session.stored.worktreePath);
        if (where == folder || where.startsWith(folder + QLatin1Char('/')))
            found.append(session.stored.id);
    }
    return found;
}

void SessionHost::teamStorageCommand(const QJsonObject& request, CommandCallback done) {
    const QString type = request.value(QStringLiteral("message_type")).toString();
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const qint64 teamId = payload.value(QStringLiteral("team_id")).toString().toLongLong();
    if (!teams_ || !teams_->isLoaded()) {
        done(refusedAnswer({QStringLiteral("teams_unavailable"),
                            QStringLiteral("The teams could not be read. Try again.")}));
        return;
    }
    if (type == QStringLiteral("team.storage.get")) {
        teams_->storage(teamId,
                        [done = std::move(done)](const std::optional<teams::TeamError>& error,
                                                 const QJsonObject& storage) {
                            done(error ? refusedAnswer(*error)
                                       : QJsonObject{{QStringLiteral("ok"), true},
                                                     {QStringLiteral("storage"), storage}});
                        });
        return;
    }
    teams_->removeWorktree(
        teamId, payload.value(QStringLiteral("task_id")).toString(), QStringLiteral("user"),
        [guard = QPointer<SessionHost>(this),
         done = std::move(done)](const teams::TeamResult& result) {
            if (!result.ok()) {
                done(refusedAnswer(*result.error));
                return;
            }
            const teams::Team* team = guard ? guard->teams_->team(result.teamId) : nullptr;
            done({{QStringLiteral("ok"), true},
                  {QStringLiteral("team"), team ? guard->teams_->teamJson(*team) : QJsonObject{}}});
        });
}

} // namespace ketplus::daemon
