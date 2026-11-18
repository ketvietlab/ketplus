#pragma once

#include "peer/PeerPorts.h"
#include "state/RunnerState.h"

// The runner's key signs its offers; its state directory holds the desktops it trusts.
namespace ketplus::runner::app {

class IdentitySigner final : public peer::OfferSigner {
  public:
    explicit IdentitySigner(const RunnerIdentity& identity) : m_identity(identity) {}
    [[nodiscard]] QString runnerId() const override { return m_identity.credential.runnerId; }
    [[nodiscard]] int runnerKeyVersion() const override { return m_identity.credential.keyVersion; }
    [[nodiscard]] QByteArray sign(const QByteArray& bytes) const override;

  private:
    const RunnerIdentity& m_identity;
};

// The active pin of a desktop, read from disk on each lookup so a pin changed by another
// command counts at once.
class StatePins final : public peer::PinLookup {
  public:
    explicit StatePins(const RunnerState& state) : m_state(state) {}
    [[nodiscard]] std::optional<pairing::RunnerPin>
    activePin(const QString& machineId) const override;

  private:
    const RunnerState& m_state;
};

} // namespace ketplus::runner::app
