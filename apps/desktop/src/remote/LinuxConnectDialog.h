#pragma once

#include <QString>

#include <functional>

class QWidget;
namespace ket {
class Dialog;
}
namespace ketplus::remote {
class ControlV2Client;
}

namespace ketplus {

class LinuxMachineModel;

// "Connect a Linux machine" (#419 scope item 4): SSH (free) or the ketPlus Relay (Pro).
//
// SSH takes a ~/.ssh/config alias or user@host[:port] and asks the model to connect. A host
// not in known_hosts comes back as a host key to accept or reject before anything is trusted.
// No mode has a password field: SSH authenticates with the user's keys or agent (ADR 0119).
//
// Until the model's backends are attached (T-6) only the Relay mode is offered.
//
// The Relay mode is the existing pairing flow (a one-time ketplus:// link from ketplusctl and
// the claim digest check). Without the Pro entitlement it explains that instead of pairing.
struct LinuxConnectHooks final {
    // The Relay needs a ketPlus account; called when the user asks to sign in.
    std::function<void()> signIn;
    // A Relay machine was trusted or an SSH machine connected.
    std::function<void(const QString& message)> finished;
};

enum class LinuxConnectMode { Ssh, Relay };

// Opens the card on `anchor`'s window; it deletes itself when it closes.
ket::Dialog* openLinuxConnectDialog(QWidget* anchor, LinuxMachineModel* model,
                                    remote::ControlV2Client* client, LinuxConnectHooks hooks,
                                    LinuxConnectMode mode = LinuxConnectMode::Ssh);

} // namespace ketplus
