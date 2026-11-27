#include "run/RunIdentity.h"

#include "pairing/PairingSignatures.h"

namespace ketplus::runner::app {

QByteArray IdentitySigner::sign(const QByteArray& bytes) const {
    return pairing::signPayload(m_identity.privateKey, bytes);
}

std::optional<pairing::RunnerPin> StatePins::activePin(const QString& machineId) const {
    QString error;
    const std::optional<QList<pairing::RunnerPin>> pins = m_state.loadPins(&error);
    if (!pins)
        return std::nullopt;
    for (const pairing::RunnerPin& pin : *pins)
        if (pin.machineId == machineId && pin.state == pairing::RunnerPin::State::Active)
            return pin;
    return std::nullopt;
}

} // namespace ketplus::runner::app
