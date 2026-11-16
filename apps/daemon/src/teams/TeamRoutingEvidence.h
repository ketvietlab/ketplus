#pragma once

#include "teams/Team.h"

#include <QDateTime>
#include <QHash>

namespace ketplus::daemon::teams {

// Host-only producer. It executes a fixed, read-only recipe; no caller check names,
// commands, risk assertions or serialized VerifiedEvidence enter this boundary.
class RoutingEvidence final {
  public:
    struct Result final {
        std::optional<risk::VerifiedEvidence> evidence;
        QString reason;
        QString recipe;
    };
    [[nodiscard]] static Result produce(const Team& team, const Task& task,
                                        const QString& revision);
    void retain(const Team& team, const Task& task, const risk::VerifiedEvidence& evidence);
    [[nodiscard]] std::optional<risk::VerifiedEvidence> lookup(const Team& team, const Task& task,
                                                               const QString& id) const;

  private:
    struct Receipt final {
        risk::VerifiedEvidence evidence;
        QString worktree;
        QString branch;
        QString base;
        QString repository;
        QString integration;
        qint64 teamRevision{0};
        QDateTime expires;
    };
    QHash<QString, Receipt> receipts_;
};

} // namespace ketplus::daemon::teams
