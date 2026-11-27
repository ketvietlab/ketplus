#pragma once

#include "remote/LinuxMachineModel.h"

class QFrame;
class QWidget;

namespace ketplus {

// One Linux machine card in Connections, as the mock's DeviceCard: icon, name with its state
// and route badges, the detail line, an actionable error, Details and Connect or Disconnect.
// The buttons only emit intents on the model, and only once its backends are attached.
[[nodiscard]] QFrame* makeLinuxMachineRow(const LinuxMachine& machine, LinuxMachineModel* model,
                                          QWidget* parent);

// Details for the model's machine `id`: the route, the verified fingerprint, the state and any
// error, with Connect and Disconnect. It follows the model while open and closes if the machine
// goes. An in-window card on `anchor`'s window; it deletes itself when it closes.
void showLinuxMachineDetails(const QString& id, LinuxMachineModel* model, QWidget* anchor);

// The state badge text: an error names what failed, otherwise the ADR 0119 state. Without
// attached backends a Relay machine is "Available": listed by the Relay, its state unknown.
[[nodiscard]] QString linuxMachineBadgeText(const LinuxMachine& machine,
                                            bool backendsAttached = true);

} // namespace ketplus
