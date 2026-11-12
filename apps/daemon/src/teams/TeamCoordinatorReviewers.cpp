// T-23: a team has one or two reviewers (ADR 0114 §2). Each task in review goes to one of
// them: the reviewer of its last round while that one is on the team, else the idle one,
// else the one with the shorter queue. A reviewer that leaves hands its reviews to the
// other one, and a team without a reviewer keeps them waiting for the next.
#include "teams/TeamCoordinator.h"

namespace ketplus::daemon::teams {
namespace {

// What a reviewer has to do: the review in flight and the queue.
qsizetype load(const ReviewSlot& slot) {
    return slot.queue.size() + (slot.reviewing.isEmpty() ? 0 : 1);
}

ReviewSlot* leastLoaded(Team& team, const QString& except = {}) {
    ReviewSlot* chosen = nullptr;
    for (ReviewSlot& slot : team.reviews)
        if (slot.reviewer != except && (!chosen || load(slot) < load(*chosen)))
            chosen = &slot;
    return chosen;
}

} // namespace

void TeamCoordinator::queueReview(Team& team, const Task& task) {
    if (team.reviewPending(task.id))
        return;
    ReviewSlot* slot = team.reviewSlot(task.reviewer);
    if (!slot)
        slot = leastLoaded(team);
    if (slot)
        slot->queue.append(task.id);
    else
        team.reviewQueue.append(task.id);
}

void TeamCoordinator::dropReview(Team& team, const QString& taskId) {
    team.reviewQueue.removeAll(taskId);
    for (ReviewSlot& slot : team.reviews) {
        slot.queue.removeAll(taskId);
        if (slot.reviewing == taskId) {
            slot.reviewing.clear();
            slot.message.clear();
        }
    }
}

void TeamCoordinator::reviewerJoined(Team& team, const QString& sessionId) {
    if (team.reviewSlot(sessionId))
        return;
    team.reviews.append({.reviewer = sessionId});
    // What waited while the team had no reviewer is this one's now.
    team.reviews.last().queue = team.reviewQueue;
    team.reviewQueue.clear();
}

void TeamCoordinator::reviewerLeft(Team& team, const QString& sessionId) {
    const ReviewSlot* leaving = team.reviewSlot(sessionId);
    if (!leaving)
        return;
    const ReviewSlot slot = *leaving;
    team.reviews.removeIf([&](const ReviewSlot& entry) { return entry.reviewer == sessionId; });
    QStringList handed = slot.queue;
    if (!slot.reviewing.isEmpty()) {
        // The review it had not finished is sent again as the same round, first.
        if (Task* task = team.task(slot.reviewing); task && task->state == state::inReview) {
            task->round = std::max(0, task->round - 1);
            task->missingVerdicts = 0;
        }
        handed.removeAll(slot.reviewing);
        handed.prepend(slot.reviewing);
    }
    for (Task& task : team.tasks)
        if (task.reviewer == sessionId)
            task.reviewer.clear();
    if (handed.isEmpty())
        return;
    if (ReviewSlot* other = leastLoaded(team)) {
        // The review in flight goes first; the rest keep their order behind the other's.
        if (!slot.reviewing.isEmpty())
            other->queue.prepend(handed.takeFirst());
        other->queue.append(handed);
    } else {
        team.reviewQueue = handed + team.reviewQueue;
    }
}

QString TeamCoordinator::reviewerFor(const Team& team, const QString& sessionId) {
    if (team.reviews.size() == 1)
        return team.reviews.first().reviewer;
    // A developer's reviewer: the one with its task in review, else the one of its last round.
    QStringList found;
    for (const Task& task : team.tasks) {
        if (task.owner != sessionId || task.state == state::done)
            continue;
        QString reviewer;
        if (const ReviewSlot* slot = team.reviewSlotOf(task.id))
            reviewer = slot->reviewer;
        else if (team.reviewSlot(task.reviewer))
            reviewer = task.reviewer;
        if (!reviewer.isEmpty() && !found.contains(reviewer))
            found.append(reviewer);
    }
    return found.size() == 1 ? found.first() : QString{};
}

} // namespace ketplus::daemon::teams
