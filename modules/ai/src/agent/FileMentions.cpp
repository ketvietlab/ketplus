#include "agent/FileMentions.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QVariantMap>

#include <algorithm>

namespace ketplus::file_mentions {
namespace {

void addWithParents(const QString& file, QSet<QString>& folders, QStringList& entries) {
    entries.append(file);
    qsizetype slash = file.lastIndexOf(QLatin1Char('/'));
    while (slash > 0) {
        const QString folder = file.left(slash + 1);
        if (folders.contains(folder)) {
            break;
        }
        folders.insert(folder);
        entries.append(folder);
        slash = file.lastIndexOf(QLatin1Char('/'), slash - 1);
    }
}

bool listGitEntries(const QString& root, qsizetype maximumEntries, QStringList& entries) {
    // A GUI app launched from Finder has a minimal PATH; look where Git usually lives too.
    QString executable = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (executable.isEmpty())
        executable = QStandardPaths::findExecutable(
            QStringLiteral("git"),
            {QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin")});
    if (executable.isEmpty())
        return false;
    QProcess git;
    git.setWorkingDirectory(root);
    git.start(executable,
              {QStringLiteral("ls-files"), QStringLiteral("-z"), QStringLiteral("--cached"),
               QStringLiteral("--others"), QStringLiteral("--exclude-standard")});
    if (!git.waitForFinished(5000) || git.exitStatus() != QProcess::NormalExit ||
        git.exitCode() != 0) {
        git.kill();
        return false;
    }
    QSet<QString> folders;
    const QList<QByteArray> files = git.readAllStandardOutput().split('\0');
    for (const QByteArray& file : files) {
        if (!file.isEmpty()) {
            addWithParents(QString::fromUtf8(file), folders, entries);
        }
        if (entries.size() >= maximumEntries) {
            break;
        }
    }
    return true;
}

void listDirectoryEntries(const QString& root, qsizetype maximumEntries, QStringList& entries) {
    static const QSet<QString> skipped{QStringLiteral(".git"), QStringLiteral("node_modules"),
                                       QStringLiteral(".cache")};
    const QDir base(root);
    QStringList pending{root};
    while (!pending.isEmpty() && entries.size() < maximumEntries) {
        const QString directory = pending.takeFirst();
        const QFileInfoList children = QDir(directory).entryInfoList(
            QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
        for (const QFileInfo& child : children) {
            if (child.isDir()) {
                if (skipped.contains(child.fileName())) {
                    continue;
                }
                entries.append(base.relativeFilePath(child.filePath()) + QLatin1Char('/'));
                pending.append(child.filePath());
            } else {
                entries.append(base.relativeFilePath(child.filePath()));
            }
        }
    }
}

QString baseName(const QString& entry) {
    const QString trimmed = entry.endsWith(QLatin1Char('/')) ? entry.chopped(1) : entry;
    return trimmed.mid(trimmed.lastIndexOf(QLatin1Char('/')) + 1);
}

bool isSubsequence(const QString& needle, const QString& haystack) {
    qsizetype position = 0;
    for (const QChar character : needle) {
        position = haystack.indexOf(character, position);
        if (position < 0) {
            return false;
        }
        ++position;
    }
    return true;
}

bool isMentionBoundary(const QChar character) {
    return character.isSpace() || QStringLiteral("([{'\"").contains(character);
}

} // namespace

QStringList listEntries(const QString& root, const qsizetype maximumEntries) {
    QStringList entries;
    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        return entries;
    }
    if (!listGitEntries(root, maximumEntries, entries)) {
        entries.clear();
        listDirectoryEntries(root, maximumEntries, entries);
    }
    return entries;
}

QStringList rankEntries(const QStringList& entries, const QString& query,
                        const qsizetype maximumResults) {
    QStringList folded;
    folded.reserve(entries.size());
    for (const QString& entry : entries)
        folded.append(entry.toCaseFolded());
    return rankEntries(entries, folded, query, maximumResults);
}

QStringList rankEntries(const QStringList& entries, const QStringList& foldedEntries,
                        const QString& query, const qsizetype maximumResults) {
    if (foldedEntries.size() != entries.size())
        return {};
    const QString needle = query.trimmed().toCaseFolded();
    struct Candidate {
        int score;
        const QString* entry;
    };
    std::vector<Candidate> candidates;
    for (qsizetype index = 0; index < entries.size(); ++index) {
        const QString& entry = entries.at(index);
        int score = 0;
        if (needle.isEmpty()) {
            score = int(entry.count(QLatin1Char('/')) - (entry.endsWith(QLatin1Char('/')) ? 1 : 0));
        } else {
            const QString& path = foldedEntries.at(index);
            const QString name = baseName(path);
            if (name == needle) {
                score = 0;
            } else if (name.startsWith(needle)) {
                score = 1;
            } else if (name.contains(needle)) {
                score = 2;
            } else if (path.contains(needle)) {
                score = 3;
            } else if (isSubsequence(needle, path)) {
                score = 4;
            } else {
                continue;
            }
        }
        candidates.push_back({score, &entry});
    }
    const auto better = [](const Candidate& left, const Candidate& right) {
        if (left.score != right.score) {
            return left.score < right.score;
        }
        if (left.entry->size() != right.entry->size()) {
            return left.entry->size() < right.entry->size();
        }
        return *left.entry < *right.entry;
    };
    const auto count = std::min<std::size_t>(candidates.size(), std::size_t(maximumResults));
    std::partial_sort(candidates.begin(), candidates.begin() + qsizetype(count), candidates.end(),
                      better);
    QStringList results;
    results.reserve(qsizetype(count));
    for (std::size_t index = 0; index < count; ++index) {
        results.append(*candidates[index].entry);
    }
    return results;
}

QVector<Mention> parseMentions(const QString& prompt) {
    QVector<Mention> mentions;
    for (qsizetype index = 0; index < prompt.size(); ++index) {
        if (prompt.at(index) != QLatin1Char('@') ||
            (index > 0 && !isMentionBoundary(prompt.at(index - 1)))) {
            continue;
        }
        Mention mention;
        mention.start = index;
        if (index + 1 < prompt.size() && prompt.at(index + 1) == QLatin1Char('"')) {
            const qsizetype close = prompt.indexOf(QLatin1Char('"'), index + 2);
            if (close < 0) {
                continue;
            }
            mention.path = prompt.mid(index + 2, close - index - 2);
            mention.end = close + 1;
        } else {
            qsizetype end = index + 1;
            while (end < prompt.size() && !prompt.at(end).isSpace()) {
                ++end;
            }
            // Sentence punctuation after a mention is not part of the path.
            while (end > index + 1 && QStringLiteral(".,;:!?)]}'\"").contains(prompt.at(end - 1))) {
                --end;
            }
            mention.path = prompt.mid(index + 1, end - index - 1);
            mention.end = end;
        }
        if (!mention.path.isEmpty()) {
            mentions.append(mention);
            index = mention.end - 1;
        }
    }
    return mentions;
}

ResolvedMention resolve(const QString& root, const QString& path) {
    ResolvedMention result;
    const QString rootPath = QFileInfo(root).canonicalFilePath();
    if (rootPath.isEmpty() || path.isEmpty()) {
        return result;
    }
    QString candidate;
    if (path == QStringLiteral("~") || path.startsWith(QStringLiteral("~/"))) {
        candidate = QDir::home().filePath(path.mid(2));
    } else if (QDir::isAbsolutePath(path)) {
        candidate = path;
    } else {
        const QString cleaned = QDir::cleanPath(path);
        candidate = QDir(rootPath).filePath(cleaned);
    }
    const QFileInfo info(candidate);
    // Text like `@/api/users` or `@../handle` names nothing on disk; it is not a path to block.
    if (!info.exists()) {
        result.resolution = Resolution::Missing;
        return result;
    }
    // Canonical paths follow symlinks, so a link inside the worktree cannot reach outside it.
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty() ||
        (canonical != rootPath && !canonical.startsWith(rootPath + QLatin1Char('/')))) {
        result.resolution = Resolution::Outside;
        return result;
    }
    result.resolution = Resolution::Inside;
    result.directory = info.isDir();
    result.relativePath = canonical == rootPath ? QString{} : canonical.mid(rootPath.size() + 1);
    if (result.directory && !result.relativePath.isEmpty()) {
        result.relativePath += QLatin1Char('/');
    }
    return result;
}

QString referencedPathInstructions(const QVariantList& paths) {
    QStringList lines;
    for (const QVariant& value : paths) {
        const QVariantMap item = value.toMap();
        const QString path = item.value(QStringLiteral("path")).toString();
        if (!path.isEmpty()) {
            lines.append(QStringLiteral("- `%1` (%2)")
                             .arg(path, item.value(QStringLiteral("directory")).toBool()
                                            ? QStringLiteral("folder")
                                            : QStringLiteral("file")));
        }
    }
    if (lines.isEmpty()) {
        return {};
    }
    return QStringLiteral("The user referenced these paths in the current worktree (relative to "
                          "its root). Read them as needed:\n%1")
        .arg(lines.join(QLatin1Char('\n')));
}

QString insertionText(const QString& relativePath) {
    const bool needsQuotes = std::any_of(relativePath.cbegin(), relativePath.cend(),
                                         [](const QChar character) { return character.isSpace(); });
    return needsQuotes ? QStringLiteral("@\"%1\"").arg(relativePath)
                       : QStringLiteral("@%1").arg(relativePath);
}

} // namespace ketplus::file_mentions
