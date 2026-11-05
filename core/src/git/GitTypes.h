#pragma once

#include <QString>
#include <QVector>

namespace ketplus {

enum class GitDiffMode {
    Combined,
    Staged,
    Unstaged,
};

struct GitFileStatus final {
    QString path;
    QString originalPath;
    QChar indexCode{QLatin1Char('.')};
    QChar worktreeCode{QLatin1Char('.')};
    bool untracked{false};
    bool ignored{false};

    [[nodiscard]] bool hasStagedChange() const noexcept {
        return !ignored && !untracked && indexCode != QLatin1Char('.') &&
               indexCode != QLatin1Char(' ');
    }

    [[nodiscard]] bool hasUnstagedChange() const noexcept {
        return !ignored &&
               (untracked || (worktreeCode != QLatin1Char('.') &&
                              worktreeCode != QLatin1Char(' ')));
    }
};

struct GitSnapshot final {
    QString repositoryRoot;
    QString head;
    QString branch;
    QString upstream;
    int ahead{0};
    int behind{0};
    QVector<GitFileStatus> files;

    [[nodiscard]] bool isRepository() const noexcept { return !repositoryRoot.isEmpty(); }
};

struct GitWorktree final {
    QString path;
    QString head;
    QString branch;
    QString lockReason;
    bool current{false};
    bool detached{false};
    bool bare{false};
    bool prunable{false};
};

} // namespace ketplus
