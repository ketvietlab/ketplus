#pragma once

// Git off the event loop, for the broker's turn reviews (ADR 0067) and its branch check
// around a Codex review turn (ketplus#295).
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class QObject;

namespace ketplus::daemon {

// Runs git in `directory`; `done` receives stdout, or nothing when git failed or ran past its
// timeout. With `indexFile`, git uses that index instead of the worktree's own. Git never
// prompts for credentials.
void runGit(QObject* owner, const QString& directory, const QStringList& arguments,
            std::function<void(std::optional<QString>)> done, const QString& indexFile = {});

} // namespace ketplus::daemon
