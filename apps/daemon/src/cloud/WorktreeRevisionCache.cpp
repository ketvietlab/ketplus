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

QByteArray readMarker(const QString& path) {
    if (!QFileInfo(path).isFile())
        return {};
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readLine(4096).trimmed() : QByteArray{};
}

void addHead(QByteArray* out, const QDir& git) {
    QByteArray head = readMarker(git.filePath(QStringLiteral("HEAD")));
    out->append(head);
    out->append('\0');
    if (!head.startsWith("ref: refs/"))
        return;

    const QString commonMarker = QString::fromUtf8(readMarker(git.filePath("commondir")));
    const QDir common(commonMarker.isEmpty() ? git.absolutePath()
                                             : git.absoluteFilePath(commonMarker));
    // A ref can itself be symbolic. Bound the walk just as Git does.
    for (int depth = 0; depth < 5 && head.startsWith("ref: refs/"); ++depth) {
        const QString ref = QString::fromUtf8(head.mid(5));
        if (QDir::cleanPath(ref) != ref || ref.contains(u'\\') || ref.contains(u'\0'))
            break;
        head = readMarker(common.filePath(ref));
        out->append(head);
        out->append('\0');
    }
    // Packing or deleting a branch may leave no loose ref to read.
    addStamp(out, common.filePath(QStringLiteral("packed-refs")));
    out->append(readMarker(common.filePath(QStringLiteral("reftable/tables.list"))));
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
        const QDir metadata(git);
        // A ref can move even when HEAD, its reflog and the index are all unchanged.
        addHead(&out, metadata);
        addStamp(&out, metadata.filePath(QStringLiteral("logs/HEAD")));
        addStamp(&out, metadata.filePath(QStringLiteral("index")));
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
