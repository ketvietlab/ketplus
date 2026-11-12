#pragma once

#include "remote/LinuxMachineModel.h"

class QFrame;
class QWidget;

namespace ketplus {

// One Linux machine card in Connections, as the mock's DeviceCard: icon, name with its state
// and route badges, the detail line, an actionable error, Details and Connect or Disconnect.
// The buttons only emit intents on the model.
[[nodiscard]] QFrame* makeLinuxMachineRow(const LinuxMachine& machine, LinuxMachineModel* model,
                                          QWidget* parent);

// Details: the route, the verified fingerprint, the state and any error, with Connect and
// Disconnect. An in-window card on `anchor`'s window; it deletes itself when it closes.
void showLinuxMachineDetails(const LinuxMachine& machine, LinuxMachineModel* model,
                             QWidget* anchor);

// The state badge text: an error names what failed, otherwise the ADR 0118 state.
[[nodiscard]] QString linuxMachineBadgeText(const LinuxMachine& machine);

} // namespace ketplus
