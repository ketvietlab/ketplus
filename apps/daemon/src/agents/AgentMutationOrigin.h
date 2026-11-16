#pragma once

#include <QString>
#include <optional>

namespace ketplus::daemon {
// Private host authority. None of these fields are model arguments or transcript data.
struct AgentMutationSnapshot final {
    QString sessionId;
    qint64 generation{0};
    qint64 turn{0};
    QString executionPolicy;
    QString transportEpoch;
};
struct AgentMutationGrant final {
    AgentMutationSnapshot snapshot;
    QString credential;
};
inline constexpr auto mutationOriginField = "_ketplus_mutation_origin";

class AgentMutationOrigin final {
  public:
    struct Bootstrap final {
        QString transportEpoch;
        QString credential;
    };
    // Reserves immutable bootstrap credentials, invalidating any previous turn.
    // A pending credential has no mutation authority until the adapter barrier ACK.
    Bootstrap prepare(const QString& session, qint64 generation, qint64 turn, const QString& policy,
                      bool review);
    bool activate(const QString& acknowledgedEpoch);
    void revoke();
    [[nodiscard]] std::optional<AgentMutationSnapshot>
    authorize(const QString& credential, const QString& session, qint64 generation) const;
    [[nodiscard]] Bootstrap bootstrap() const { return bootstrap_; }

  private:
    Bootstrap bootstrap_;
    AgentMutationSnapshot snapshot_;
    bool active_{false};
};
} // namespace ketplus::daemon
