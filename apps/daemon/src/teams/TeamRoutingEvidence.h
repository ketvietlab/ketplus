#pragma once

#include "teams/Team.h"

#include <QHash>
#include <functional>

namespace ketplus::daemon::teams {

// Host-only producer. It executes a fixed, read-only recipe; no caller check names,
// commands, risk assertions or serialized VerifiedEvidence enter this boundary.
class RoutingEvidence final {
  public:
    using Clock = std::function<qint64()>;
    explicit RoutingEvidence(Clock clock = {});
    struct Result final {
        std::optional<risk::VerifiedEvidence> evidence;
        QString reason;
        QString recipe;
    };
    [[nodiscard]] static Result produce(const Team& team, const Task& task,
                                        const QString& revision);
    // Content/policy binding, independent of unrelated team activity and note revisions.
    [[nodiscard]] static QString semanticRevision(const Team& team, const Task& task,
                                                  const QString& revision);
    bool retain(const Team& team, const Task& task, const risk::VerifiedEvidence& evidence);
    [[nodiscard]] std::optional<risk::VerifiedEvidence> lookup(const Team& team, const Task& task,
                                                               const QString& id) const;

  private:
    struct Receipt final {
        risk::VerifiedEvidence evidence;
        QString worktree;
        QString branch;
        QString semanticRevision;
        qint64 expires{0};
        quint64 order{0};
    };
    void pruneExpired(qint64 now) const;
    mutable QHash<QString, Receipt> receipts_;
    Clock clock_;
    quint64 order_{0};
};

} // namespace ketplus::daemon::teams
