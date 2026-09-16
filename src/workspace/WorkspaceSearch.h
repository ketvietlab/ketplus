#pragma once

#include "workspace/WorkspaceFileIndex.h"

#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QString>

#include <atomic>
#include <functional>

namespace ketplus {

struct WorkspaceSearchOptions final {
    QString query;
    bool matchCase{false};
    bool wholeWord{false};
    bool regex{false};
    // Comma-separated globs such as "*.cpp, src/**"; empty searches every file.
    QString includePatterns;

    bool operator==(const WorkspaceSearchOptions&) const = default;
};

struct WorkspaceSearchMatch final {
    int line{0};    // 1-based
    int column{0};  // 0-based, in UTF-16 characters
    int length{0};  // in UTF-16 characters
    QString preview;
};

struct WorkspaceSearchFileResult final {
    QString path;
    QString relativePath;
    QList<WorkspaceSearchMatch> matches;
};

struct WorkspaceSearchSummary final {
    int fileCount{0};
    int matchCount{0};
    bool truncated{false};
};

inline constexpr qint64 maximumSearchFileBytes = 5 * 1024 * 1024;
inline constexpr int defaultSearchMatchLimit = 10000;
inline constexpr int maximumSearchPreviewLength = 300;

// Builds the expression for a query. On failure returns an invalid expression and sets `error`.
[[nodiscard]] QRegularExpression buildSearchExpression(const WorkspaceSearchOptions& options,
                                                       QString* error);
[[nodiscard]] bool matchesIncludePatterns(const QString& relativePath, const QString& patterns);
// Matches line by line, so patterns never span line breaks.
[[nodiscard]] QList<WorkspaceSearchMatch> searchText(const QString& text,
                                                     const QRegularExpression& expression,
                                                     int maximumMatches);
// Reads a file for searching. Returns false for missing, oversized, binary or non-UTF-8 files.
[[nodiscard]] bool readSearchableFile(const QString& path, QString* text);
// Replaces matches line by line and keeps line endings. With `regex`, \1 or $1 insert groups.
[[nodiscard]] QString replaceInText(const QString& text, const QRegularExpression& expression,
                                    const QString& replacement, bool regex, int* replacements);

using WorkspaceSearchFileCallback = std::function<void(const WorkspaceSearchFileResult&)>;

// Scans workspace files from disk without any content index. `openBuffers` maps canonical
// absolute paths to unsaved editor text that replaces the file on disk. Safe to call from
// a worker thread; `onFile` runs on that thread.
[[nodiscard]] WorkspaceSearchSummary searchWorkspace(
    const QString& rootPath, const WorkspaceSearchOptions& options,
    const QRegularExpression& expression, const QHash<QString, QString>& openBuffers,
    const std::atomic_bool* cancelled, const WorkspaceSearchFileCallback& onFile,
    int maximumMatches = defaultSearchMatchLimit,
    int maximumFiles = defaultWorkspaceFileLimit);

} // namespace ketplus
