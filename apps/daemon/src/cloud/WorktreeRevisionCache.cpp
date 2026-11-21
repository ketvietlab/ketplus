#include "cloud/WorktreeRevisionCache.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTimeZone>

#include <algorithm>

namespace ketplus::daemon {

namespace {

// The git directory of a worktree: `.git` itself, or the directory a linked worktree's `.git`
// file names. A workspace may be a folder inside a repository, so this walks up as
// ReadOnlyWorkspaceFiles does.
QString gitDirectory(const QString& worktreePath) {
    if (!QFileInfo(worktreePath).isDir())
        return {};
    for (QDir directory(worktreePath);;) {
        const QString dotGit = directory.filePath(QStringLiteral(".git"));
        const QFileInfo info(dotGit);
        if (info.isDir())
            return dotGit;
        if (info.isFile()) {
            QFile file(dotGit);
            if (!file.open(QIODevice::ReadOnly))
                return {};
            const QByteArray line = file.readLine(4096).trimmed();
            if (!line.startsWith("gitdir:"))
                return {};
            return directory.absoluteFilePath(QString::fromUtf8(line.mid(7).trimmed()));
        }
        if (!directory.cdUp())
            return {};
    }
}

void addStamp(QByteArray* out, const QString& path) {
    const QFileInfo info(path);
    out->append(path.toUtf8());
    out->append('\0');
    if (!info.exists()) {
        out->append("missing");
    } else {
        out->append(QByteArray::number(info.size()));
        out->append(':');
        out->append(QByteArray::number(info.lastModified(QTimeZone::UTC).toMSecsSinceEpoch()));
        out->append(':');
        out->append(
            QByteArray::number(info.metadataChangeTime(QTimeZone::UTC).toMSecsSinceEpoch()));
    }
    out->append('\0');
}

} // namespace

QByteArray WorktreeRevisionCache::fingerprint(const QString& worktreePath,
                                              const QList<StoredSession>& sessions) {
    QList<const StoredSession*> ordered;
    for (const StoredSession& session : sessions)
        ordered.append(&session);
    std::sort(ordered.begin(), ordered.end(),
              [](const StoredSession* a, const StoredSession* b) { return a->id < b->id; });
    QByteArray out;
    for (const StoredSession* session : ordered) {
        out.append(session->id.toUtf8());
        out.append('\0');
        out.append(QByteArray::number(session->generation));
        out.append(':');
        out.append(QByteArray::number(session->lastCursor));
        out.append(':');
        out.append(session->state.toUtf8());
        out.append('\0');
    }
    const QString git = gitDirectory(worktreePath);
    if (git.isEmpty()) {
        out.append(QFileInfo(worktreePath).isDir() ? "no-git" : "no-worktree");
    } else {
        // HEAD changes on checkout; its reflog on every move of HEAD, also one that leaves the
        // index alone (a commit with nothing staged, reset --soft); the index on staging.
        addStamp(&out, QDir(git).filePath(QStringLiteral("HEAD")));
        addStamp(&out, QDir(git).filePath(QStringLiteral("logs/HEAD")));
        addStamp(&out, QDir(git).filePath(QStringLiteral("index")));
    }
    return out;
}

QString WorktreeRevisionCache::revision(const QString& worktreePath,
                                        const QList<StoredSession>& sessions, const qint64 nowMs,
                                        const Compute& compute) {
    const QByteArray current = fingerprint(worktreePath, sessions);
    const auto found = entries_.constFind(worktreePath);
    if (found != entries_.constEnd() && found->fingerprint == current &&
        nowMs - found->computedAtMs < maximumAgeMs && nowMs >= found->computedAtMs)
        return found->revision;
    const QString revision = compute(worktreePath);
    // An empty revision from a repository is a failed git run (an error or a timeout); keep it
    // for this heartbeat only. Outside a repository empty is the answer.
    if (revision.isEmpty() && !gitDirectory(worktreePath).isEmpty())
        entries_.remove(worktreePath);
    else
        entries_.insert(worktreePath, {current, revision, nowMs});
    return revision;
}

void WorktreeRevisionCache::retain(const QList<StoredSession>& stored) {
    QSet<QString> used;
    for (const StoredSession& session : stored)
        used.insert(session.worktreePath);
    entries_.removeIf(
        [&](const QHash<QString, Entry>::iterator entry) { return !used.contains(entry.key()); });
}

} // namespace ketplus::daemon
