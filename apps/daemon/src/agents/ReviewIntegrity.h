#pragma once

// ketplus#295 (ADR 0067, ADR 0114 §2): a Codex reviewer reviews with its own Full access, so the
// daemon records the reviewed branch before each of its review turns and compares it after.
// Any change voids the verdict; nothing is reverted.
#include <QJsonArray>
#include <QString>

#include <functional>

class QObject;

namespace ketplus::daemon::review_integrity {

// What one review request reviews.
struct Scope final {
    // The reviewed worktree; empty when there is none to check locally.
    QString worktree;
    // A team task's branch, read on the remote with `git ls-remote origin`; empty off a team.
    QString branch;
    // Where `git ls-remote` runs: the worktree, or the team's repository without one.
    QString remoteDirectory;
    // A team task's head_commit on the board.
    QString taskHead;
    // The conversation that does the reviewed work, told when the review is voided.
    QString workingSession;
};

struct Snapshot final {
    bool local{false};
    QString head;
    QString symbolicRef;
    // SHA-256 of `git status --porcelain=v2 --untracked-files=all`.
    QString statusHash;
    QString taskHead;
    // `git ls-remote` answered; a network failure is noted, never fatal.
    bool remote{false};
    QString remoteHead;
};

// Records `scope` off the event loop; `done` runs once, in `owner`'s thread.
void capture(QObject* owner, const Scope& scope, std::function<void(Snapshot)> done);

// What changed, as [{"field", "before", "after"}]; empty when nothing did. A remote that did not
// answer before or after is not compared.
[[nodiscard]] QJsonArray changes(const Snapshot& before, const Snapshot& after);

// Whether the remote branch head could be compared.
[[nodiscard]] bool remoteCompared(const Scope& scope, const Snapshot& before,
                                  const Snapshot& after);

// One line per change, for team history and the leader: "HEAD: abc → def".
[[nodiscard]] QString describe(const QJsonArray& changes);

// The review role for a Codex reviewer, put before the request in its prompt.
[[nodiscard]] QString codexReviewInstructions();

} // namespace ketplus::daemon::review_integrity
