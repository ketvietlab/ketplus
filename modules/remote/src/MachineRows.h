#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace ketplus::remote {

// ADR 0119: the rows Desktop shows for one machine, read from MachineBackend results. Each route
// keeps its own result spelling (the local protocol's `session_id`, the Relay projection's `id`);
// these readers accept both so no consumer branches on the route.
struct MachineWorktree final {
    QString id; // what createSession() and listWorktrees() name: the host path, or a Relay id
    QString label;
    QString path; // only where the host path may be shown
    QString branch;
    QString repositoryId;
    bool available{true};
    // Listed by the route but not yet verified (the Relay's lazy host check); filled in on the
    // next listing after MachineBackend::catalogChanged().
    bool pending{false};

    friend bool operator==(const MachineWorktree&, const MachineWorktree&) = default;
};

struct MachineRepository final {
    QString id;
    QString label;
    QString path;
    bool available{true};
    QVector<MachineWorktree> worktrees;

    friend bool operator==(const MachineRepository&, const MachineRepository&) = default;
};

struct MachineConversation final {
    QString sessionId;
    qint64 generation{0};
    QString title;
    QString number;
    QString worktreeId;
    QString worktreeLabel;
    QString repositoryId; // empty when the route names only the worktree
    QString state;
    bool awaitingReply{false};

    [[nodiscard]] bool running() const {
        return state == QLatin1String("running") || state == QLatin1String("starting");
    }
    friend bool operator==(const MachineConversation&, const MachineConversation&) = default;
};

// listRepositories(): repositories with their worktrees. A row without `worktrees` (an older
// adapter) is one worktree with the repository's own id.
[[nodiscard]] QVector<MachineRepository> repositoriesFromListing(const QJsonObject& result);
// The listing's `pending` rows: worktrees the route still verifies. They carry no repository.
[[nodiscard]] QVector<MachineWorktree> pendingWorktreesFromListing(const QJsonObject& result);
// listWorktrees(): one page of the worktree picker.
[[nodiscard]] QVector<MachineWorktree> worktreesFromPage(const QJsonObject& result);
// listConversations(): the conversation list.
[[nodiscard]] QVector<MachineConversation> conversationsFromListing(const QJsonObject& result);
// A write the route queued ({"status": "queued", "command_id"}): its command id, whose outcome
// arrives through MachineBackend::commandCompleted(). Empty for any other result.
[[nodiscard]] QString queuedCommandId(const QJsonObject& result);
// A session event in a transcript window's shape: `event_type` beside `session_id`,
// `session_generation`, `cursor` and `turn_sequence`, with the event's data as `payload`.
// eventReceived() delivers the session.event shape instead, which nests them as
// `payload {event_type, data}`; this reads either.
[[nodiscard]] QJsonObject windowEvent(const QJsonObject& event);
// createSession(): the new conversation's id and generation.
[[nodiscard]] MachineConversation createdSession(const QJsonObject& result);

} // namespace ketplus::remote
