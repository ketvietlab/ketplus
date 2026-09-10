#include "git/GitParser.h"

#include <QDir>
#include <QFileInfo>
#include <QList>

namespace ketplus::git_parser {
namespace {

QString pathAfterFields(const QByteArray& record, const int fieldCount) {
    qsizetype position = 0;
    for (int field = 0; field < fieldCount; ++field) {
        position = record.indexOf(' ', position);
        if (position < 0) {
            return {};
        }
        ++position;
    }
    return QString::fromUtf8(record.mid(position));
}

QString normalizedPath(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo file(path);
    const QString canonical = file.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? file.absoluteFilePath() : canonical);
}

void appendWorktree(QVector<GitWorktree>& worktrees, GitWorktree& worktree,
                    const QString& currentRoot) {
    if (worktree.path.isEmpty()) {
        return;
    }
    worktree.path = normalizedPath(worktree.path);
    worktree.current = worktree.path == normalizedPath(currentRoot);
    worktrees.append(worktree);
    worktree = {};
}

} // namespace

GitSnapshot parseStatus(const QByteArray& output, const QString& repositoryRoot) {
    GitSnapshot snapshot;
    snapshot.repositoryRoot = normalizedPath(repositoryRoot);

    const QList<QByteArray> records = output.split('\0');
    for (qsizetype index = 0; index < records.size(); ++index) {
        const QByteArray& record = records.at(index);
        if (record.isEmpty()) {
            continue;
        }

        if (record.startsWith("# branch.oid ")) {
            snapshot.head = QString::fromUtf8(record.mid(13));
        } else if (record.startsWith("# branch.head ")) {
            snapshot.branch = QString::fromUtf8(record.mid(14));
        } else if (record.startsWith("# branch.upstream ")) {
            snapshot.upstream = QString::fromUtf8(record.mid(18));
        } else if (record.startsWith("# branch.ab ")) {
            const QList<QByteArray> counts = record.mid(12).split(' ');
            if (counts.size() >= 2) {
                snapshot.ahead = counts.at(0).mid(1).toInt();
                snapshot.behind = counts.at(1).mid(1).toInt();
            }
        } else if (record.startsWith("1 ")) {
            snapshot.files.append({
                .path = pathAfterFields(record, 8),
                .indexCode = QChar::fromLatin1(record.at(2)),
                .worktreeCode = QChar::fromLatin1(record.at(3)),
            });
        } else if (record.startsWith("2 ")) {
            GitFileStatus status{
                .path = pathAfterFields(record, 9),
                .indexCode = QChar::fromLatin1(record.at(2)),
                .worktreeCode = QChar::fromLatin1(record.at(3)),
            };
            if (index + 1 < records.size()) {
                status.originalPath = QString::fromUtf8(records.at(++index));
            }
            snapshot.files.append(status);
        } else if (record.startsWith("u ")) {
            snapshot.files.append({
                .path = pathAfterFields(record, 10),
                .indexCode = QChar::fromLatin1(record.at(2)),
                .worktreeCode = QChar::fromLatin1(record.at(3)),
            });
        } else if (record.startsWith("? ")) {
            snapshot.files.append({
                .path = QString::fromUtf8(record.mid(2)),
                .indexCode = QLatin1Char('?'),
                .worktreeCode = QLatin1Char('?'),
                .untracked = true,
            });
        } else if (record.startsWith("! ")) {
            snapshot.files.append({
                .path = QString::fromUtf8(record.mid(2)),
                .indexCode = QLatin1Char('!'),
                .worktreeCode = QLatin1Char('!'),
                .ignored = true,
            });
        }
    }

    if (snapshot.branch == QStringLiteral("(detached)")) {
        snapshot.branch.clear();
    }
    return snapshot;
}

QVector<GitWorktree> parseWorktrees(const QByteArray& output, const QString& currentRoot) {
    QVector<GitWorktree> worktrees;
    GitWorktree current;

    const QList<QByteArray> fields = output.split('\0');
    for (const QByteArray& field : fields) {
        if (field.isEmpty()) {
            appendWorktree(worktrees, current, currentRoot);
        } else if (field.startsWith("worktree ")) {
            if (!current.path.isEmpty()) {
                appendWorktree(worktrees, current, currentRoot);
            }
            current.path = QString::fromUtf8(field.mid(9));
        } else if (field.startsWith("HEAD ")) {
            current.head = QString::fromUtf8(field.mid(5));
        } else if (field.startsWith("branch ")) {
            current.branch = QString::fromUtf8(field.mid(7));
            if (current.branch.startsWith(QStringLiteral("refs/heads/"))) {
                current.branch = current.branch.mid(11);
            }
        } else if (field == "detached") {
            current.detached = true;
        } else if (field == "bare") {
            current.bare = true;
        } else if (field.startsWith("locked")) {
            current.lockReason = QString::fromUtf8(field.mid(6)).trimmed();
        } else if (field.startsWith("prunable")) {
            current.prunable = true;
        }
    }
    appendWorktree(worktrees, current, currentRoot);
    return worktrees;
}

} // namespace ketplus::git_parser
