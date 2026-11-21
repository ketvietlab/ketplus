#include "state/PinTrust.h"

#include "state/RunnerState.h"

namespace ketplus::runner::app {

bool PinTrust::trusted(const QString& machineId) const {
    QString error;
    const std::optional<QList<pairing::RunnerPin>> pins = m_state.loadPins(&error);
    if (!pins)
        return false;
    for (const pairing::RunnerPin& pin : *pins)
        if (pin.machineId == machineId && pin.state == pairing::RunnerPin::State::Active)
            return true;
    return false;
}

} // namespace ketplus::runner::app
