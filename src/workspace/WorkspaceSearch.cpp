#include "workspace/WorkspaceSearch.h"

#include "workspace/WorkspaceFileIndex.h"

#include <QDir>
#include <QFile>
#include <QStringDecoder>

namespace ketplus {
namespace {

constexpr qsizetype binaryProbeBytes = 8192;

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

QString replaceInLine(const QString& line, const QRegularExpression& expression,
                      const QString& replacement, const bool regex, int* replacements) {
    QString result;
    qsizetype copiedUpTo = 0;
    auto iterator = expression.globalMatch(line);
    while (iterator.hasNext()) {
        const auto match = iterator.next();
        result += QStringView(line).mid(copiedUpTo, match.capturedStart() - copiedUpTo);
        result += regex ? expandReplacement(replacement, match) : replacement;
        copiedUpTo = match.capturedEnd();
        ++*replacements;
    }
    if (copiedUpTo == 0 && result.isEmpty()) {
        return line;
    }
    result += QStringView(line).mid(copiedUpTo);
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
    while (lineStart <= text.size() && matches.size() < maximumMatches) {
        ++lineNumber;
        qsizetype lineEnd = text.indexOf(u'\n', lineStart);
        const bool lastLine = lineEnd < 0;
        if (lastLine) {
            lineEnd = text.size();
        }
        qsizetype contentEnd = lineEnd;
        if (contentEnd > lineStart && text.at(contentEnd - 1) == u'\r') {
            --contentEnd;
        }

        const QString line = text.mid(lineStart, contentEnd - lineStart);
        auto iterator = expression.globalMatch(line);
        while (iterator.hasNext() && matches.size() < maximumMatches) {
            const auto match = iterator.next();
            if (match.capturedLength() == 0) {
                continue;
            }
            matches.append({lineNumber, static_cast<int>(match.capturedStart()),
                            static_cast<int>(match.capturedLength()),
                            line.left(maximumSearchPreviewLength)});
        }
        if (lastLine) {
            break;
        }
        lineStart = lineEnd + 1;
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
        qsizetype lineEnd = text.indexOf(u'\n', lineStart);
        const bool lastLine = lineEnd < 0;
        if (lastLine) {
            lineEnd = text.size();
        }
        qsizetype contentEnd = lineEnd;
        if (contentEnd > lineStart && text.at(contentEnd - 1) == u'\r') {
            --contentEnd;
        }
        result += replaceInLine(text.mid(lineStart, contentEnd - lineStart), expression,
                                replacement, regex, replacements);
        result += QStringView(text).mid(contentEnd, lineEnd - contentEnd);
        if (lastLine) {
            break;
        }
        result += u'\n';
        lineStart = lineEnd + 1;
    }
    return result;
}

WorkspaceSearchSummary searchWorkspace(const QString& rootPath,
                                       const WorkspaceSearchOptions& options,
                                       const QRegularExpression& expression,
                                       const QHash<QString, QString>& openBuffers,
                                       const std::atomic_bool* cancelled,
                                       const WorkspaceSearchFileCallback& onFile,
                                       const int maximumMatches) {
    WorkspaceSearchSummary summary;
    const auto isCancelled = [cancelled] { return cancelled != nullptr && cancelled->load(); };
    const WorkspaceFileList files =
        collectWorkspaceFiles(rootPath, defaultWorkspaceFileLimit, cancelled);
    summary.truncated = files.truncated;
    const QDir root(rootPath);

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
