#pragma once

#include "git/GitTypes.h"

#include <QByteArray>

namespace ketplus::git_parser {

[[nodiscard]] GitSnapshot parseStatus(const QByteArray& output, const QString& repositoryRoot);
[[nodiscard]] QVector<GitWorktree> parseWorktrees(const QByteArray& output,
                                                  const QString& currentRoot);

} // namespace ketplus::git_parser
