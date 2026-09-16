#include "workspace/WorkspaceSearch.h"

#include "workspace/WorkspaceFileIndex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>

#include <utility>

namespace ketplus {
namespace {

constexpr qsizetype binaryProbeBytes = 8192;

// Returns the position and length of the next LF, CRLF or lone CR at or after `from`,
// or {text size, 0} when the last line has no line break.
std::pair<qsizetype, qsizetype> nextLineBreak(const QString& text, const qsizetype from) {
    for (qsizetype index = from; index < text.size(); ++index) {
        const QChar character = text.at(index);
        if (character == u'\n') {
            return {index, 1};
        }
        if (character == u'\r') {
            const bool crlf = index + 1 < text.size() && text.at(index + 1) == u'\n';
            return {index, crlf ? 2 : 1};
        }
    }
    return {text.size(), 0};
}

QRegularExpression globToExpression(const QString& glob) {
    QString pattern;
    for (qsizetype index = 0; index < glob.size(); ++index) {
        const QChar character = glob.at(index);
        if (character == u'*') {
            if (index + 1 < glob.size() && glob.at(index + 1) == u'*') {
                pattern += QStringLiteral(".*");
                ++index;
            } else {
                pattern += QStringLiteral("[^/]*");
            }
        } else if (character == u'?') {
            pattern += QStringLiteral("[^/]");
        } else {
            pattern += QRegularExpression::escape(QString(character));
        }
    }
    return QRegularExpression(QStringLiteral("^%1$").arg(pattern),
                              QRegularExpression::CaseInsensitiveOption);
}

QString expandReplacement(const QString& replacement, const QRegularExpressionMatch& match) {
    QString expanded;
    for (qsizetype index = 0; index < replacement.size(); ++index) {
        const QChar character = replacement.at(index);
        const bool hasNext = index + 1 < replacement.size();
        if ((character == u'\\' || character == u'$') && hasNext &&
            replacement.at(index + 1).isDigit()) {
            expanded += match.captured(replacement.at(index + 1).digitValue());
            ++index;
        } else if (character == u'\\' && hasNext && replacement.at(index + 1) == u'\\') {
            expanded += u'\\';
            ++index;
        } else if (character == u'$' && hasNext && replacement.at(index + 1) == u'$') {
            expanded += u'$';
            ++index;
        } else {
            expanded += character;
        }
    }
    return expanded;
}

QString replaceInLine(const QStringView line, const QRegularExpression& expression,
                      const QString& replacement, const bool regex, int* replacements) {
    QString result;
    qsizetype copiedUpTo = 0;
    auto iterator = expression.globalMatchView(line);
    while (iterator.hasNext()) {
        const auto match = iterator.next();
        // Search never lists empty matches, so replace must not touch them either;
        // otherwise a pattern like "a*" would insert text between every character.
        if (match.capturedLength() == 0) {
            continue;
        }
        result += line.mid(copiedUpTo, match.capturedStart() - copiedUpTo);
        result += regex ? expandReplacement(replacement, match) : replacement;
        copiedUpTo = match.capturedEnd();
        ++*replacements;
    }
    if (copiedUpTo == 0 && result.isEmpty()) {
        return line.toString();
    }
    result += line.mid(copiedUpTo);
    return result;
}

} // namespace

QRegularExpression buildSearchExpression(const WorkspaceSearchOptions& options, QString* error) {
    error->clear();
    if (options.query.isEmpty()) {
        *error = QStringLiteral("Type something to search for.");
        return {};
    }

    QString pattern = options.regex ? options.query : QRegularExpression::escape(options.query);
    if (options.wholeWord) {
        pattern = QStringLiteral("\\b(?:%1)\\b").arg(pattern);
    }
    QRegularExpression::PatternOptions patternOptions =
        QRegularExpression::UseUnicodePropertiesOption;
    if (!options.matchCase) {
        patternOptions |= QRegularExpression::CaseInsensitiveOption;
    }
    QRegularExpression expression(pattern, patternOptions);
    if (!expression.isValid()) {
        *error = QStringLiteral("Invalid regular expression: %1").arg(expression.errorString());
        return {};
    }
    return expression;
}

bool matchesIncludePatterns(const QString& relativePath, const QString& patterns) {
    const QStringList globs = patterns.split(QLatin1Char(','), Qt::SkipEmptyParts);
    bool hasGlob = false;
    for (QString glob : globs) {
        glob = glob.trimmed();
        if (glob.isEmpty()) {
            continue;
        }
        hasGlob = true;
        if (glob.endsWith(QLatin1Char('/'))) {
            glob += QStringLiteral("**");
        }
        // A glob without a slash matches the file name anywhere, like "*.cpp".
        const QString subject = glob.contains(QLatin1Char('/'))
                                    ? relativePath
                                    : relativePath.section(QLatin1Char('/'), -1);
        if (globToExpression(glob).match(subject).hasMatch()) {
            return true;
        }
    }
    return !hasGlob;
}

QList<WorkspaceSearchMatch> searchText(const QString& text, const QRegularExpression& expression,
                                       const int maximumMatches) {
    QList<WorkspaceSearchMatch> matches;
    qsizetype lineStart = 0;
    int lineNumber = 0;
    while (matches.size() < maximumMatches) {
        ++lineNumber;
        const auto [breakAt, breakLength] = nextLineBreak(text, lineStart);
        // Views avoid copying every line; only lines with matches allocate a preview.
        const QStringView line = QStringView(text).mid(lineStart, breakAt - lineStart);
        auto iterator = expression.globalMatchView(line);
        while (iterator.hasNext() && matches.size() < maximumMatches) {
            const auto match = iterator.next();
            if (match.capturedLength() == 0) {
                continue;
            }
            matches.append({lineNumber, static_cast<int>(match.capturedStart()),
                            static_cast<int>(match.capturedLength()),
                            line.left(maximumSearchPreviewLength).toString()});
        }
        if (breakLength == 0) {
            break;
        }
        lineStart = breakAt + breakLength;
    }
    return matches;
}

bool readSearchableFile(const QString& path, QString* text) {
    QFile file(path);
    if (file.size() > maximumSearchFileBytes || !file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray raw = file.readAll();
    if (raw.left(binaryProbeBytes).contains('\0')) {
        return false;
    }
    // Stateless, so a truncated sequence at the end of the file counts as invalid.
    QStringDecoder decoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    QString decoded = decoder.decode(raw);
    if (decoder.hasError()) {
        return false;
    }
    *text = std::move(decoded);
    return true;
}

QString replaceInText(const QString& text, const QRegularExpression& expression,
                      const QString& replacement, const bool regex, int* replacements) {
    *replacements = 0;
    QString result;
    result.reserve(text.size());
    qsizetype lineStart = 0;
    while (true) {
        const auto [breakAt, breakLength] = nextLineBreak(text, lineStart);
        result += replaceInLine(QStringView(text).mid(lineStart, breakAt - lineStart), expression,
                                replacement, regex, replacements);
        if (breakLength == 0) {
            break;
        }
        result += QStringView(text).mid(breakAt, breakLength);
        lineStart = breakAt + breakLength;
    }
    return result;
}

WorkspaceSearchSummary searchWorkspace(const QString& rootPath,
                                       const WorkspaceSearchOptions& options,
                                       const QRegularExpression& expression,
                                       const QHash<QString, QString>& openBuffers,
                                       const std::atomic_bool* cancelled,
                                       const WorkspaceSearchFileCallback& onFile,
                                       const int maximumMatches, const int maximumFiles) {
    WorkspaceSearchSummary summary;
    const auto isCancelled = [cancelled] { return cancelled != nullptr && cancelled->load(); };
    const WorkspaceFileList files = collectWorkspaceFiles(rootPath, maximumFiles, cancelled);
    summary.truncated = files.truncated;
    // Canonical paths line up with the keys of `openBuffers` even through symlinks.
    const QString canonicalRoot = QFileInfo(rootPath).canonicalFilePath();
    const QDir root(canonicalRoot.isEmpty() ? rootPath : canonicalRoot);

    for (const QString& relativePath : files.relativePaths) {
        if (isCancelled()) {
            break;
        }
        if (!matchesIncludePatterns(relativePath, options.includePatterns)) {
            continue;
        }

        const QString path = QDir::cleanPath(root.absoluteFilePath(relativePath));
        QString text;
        if (const auto buffer = openBuffers.constFind(path); buffer != openBuffers.constEnd()) {
            text = buffer.value();
        } else if (hasBinaryFileSuffix(relativePath)) {
            // Assets are recognised by name, so thousands of images are never opened.
            continue;
        } else if (!readSearchableFile(path, &text)) {
            continue;
        }

        const auto matches = searchText(text, expression, maximumMatches - summary.matchCount);
        if (matches.isEmpty()) {
            continue;
        }
        summary.matchCount += static_cast<int>(matches.size());
        ++summary.fileCount;
        if (onFile) {
            onFile({path, relativePath, matches});
        }
        if (summary.matchCount >= maximumMatches) {
            summary.truncated = true;
            break;
        }
    }
    return summary;
}

} // namespace ketplus
