#include "agents/AgentMutationOrigin.h"

#include <QUuid>

namespace ketplus::daemon {
AgentMutationOrigin::Bootstrap
AgentMutationOrigin::prepare(const QString& session, const qint64 generation, const qint64 turn,
                             const QString& policy, const bool review,
                             const bool mutationSupported) {
    revoke();
    if (session.isEmpty() || generation <= 0 || turn <= 0 ||
        (policy != "ask" && policy != "full" && policy != "read-only"))
        return {};
    bootstrap_.transportEpoch = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Review and unverified providers get a fresh read/discovery epoch, never a secret.
    if (!review && mutationSupported)
        bootstrap_.credential = QUuid::createUuid().toString(QUuid::WithoutBraces);
    snapshot_ = {session, generation, turn, policy, bootstrap_.transportEpoch};
    return bootstrap_;
}

bool AgentMutationOrigin::activate(const QString& acknowledgedEpoch) {
    if (acknowledgedEpoch.isEmpty() || acknowledgedEpoch != bootstrap_.transportEpoch)
        return false;
    active_ = !bootstrap_.credential.isEmpty();
    return true;
}

void AgentMutationOrigin::revoke() {
    active_ = false;
    bootstrap_ = {};
    snapshot_ = {};
}

std::optional<AgentMutationSnapshot> AgentMutationOrigin::authorize(const QString& credential,
                                                                    const QString& session,
                                                                    const qint64 generation) const {
    if (!active_ || credential.isEmpty() || credential != bootstrap_.credential ||
        session != snapshot_.sessionId || generation != snapshot_.generation)
        return std::nullopt;
    return snapshot_;
}
} // namespace ketplus::daemon
