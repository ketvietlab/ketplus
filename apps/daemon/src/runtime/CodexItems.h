#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace ketplus::daemon {

// Where each file a change touched sits in the session's worktree, as the worktree-relative
// `worktreePath` a client opens it by. Codex names changed files by absolute path, which no
// client can open; a file outside the worktree is flagged instead, so a client can say so
// rather than offer to open it. The provider's own `path` stays for the desktop.
[[nodiscard]] QJsonObject withWorktreeChangePaths(QJsonObject item, const QString& worktree);

// A Codex command with what it is for, the `description` a Claude command carries, so every
// client reads a command the same way: a line saying what it does above the command. Codex
// says only what it parsed, so a command it could not read (`unknown`) gains nothing.
[[nodiscard]] QJsonObject withCommandDescription(QJsonObject item);

// Codex's account/rateLimits payload as unchecked plan limit rows: the "codex" bucket (the one
// the desktop's weekly ring reads), one row per window, labelled like the Claude sidecar's.
[[nodiscard]] QJsonArray codexPlanLimits(const QJsonObject& params);

} // namespace ketplus::daemon
