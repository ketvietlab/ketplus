#pragma once

#include "remote/LinuxMachineModel.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>

#include <functional>
#include <memory>

namespace ketplus::remote {
class ControlV2Client;
class MachineBackend;
} // namespace ketplus::remote

namespace ketplus {

// One host's Relay route as Connections drives it: the ADR 0119 backend, its start and stop,
// and the commands still waiting for the host. Production wraps RelayMachineBackend; tests
// pass a fake.
class LinuxRelayLink : public QObject {
    Q_OBJECT

  public:
    using QObject::QObject;
    [[nodiscard]] virtual remote::MachineBackend* backend() = 0;
    virtual void connectToRelay() = 0;
    virtual void disconnectFromRelay() = 0;
    // Commands in the outbox whose outcome the host has not returned yet.
    [[nodiscard]] virtual int pendingCommands() const = 0;

  signals:
    // The pending commands changed (a queued command completed or failed).
    void pendingChanged();
};

// The Relay half of Connections (ADR 0119, docs/ssh-transport.md "Connections"). Every host the
// Relay's worktree index lists has one LinuxRelayLink, and its live state, pending commands and
// errors are written into the LinuxMachineModel's Relay row for that host. The links run only
// while the signed-in account is entitled to the Relay (ketPlus Pro); otherwise every Relay row
// is Offline and offers no Connect. A host that is also reached over SSH is one machine with SSH
// preferred: its Connect and Disconnect are the SSH backend's.
class LinuxRelayMachines final : public QObject {
    Q_OBJECT

  public:
    using LinkFactory = std::function<std::unique_ptr<LinuxRelayLink>(const QString& relayId)>;
    // RelayMachineBackend over `client`, with one owner-only outbox file per host.
    [[nodiscard]] static LinkFactory systemLinks(remote::ControlV2Client* client);
    // The outbox file of the host the Relay lists as `relayId`.
    [[nodiscard]] static QString outboxFile(const QString& relayId);

    LinuxRelayMachines(LinuxMachineModel* model, LinkFactory links, QObject* parent = nullptr);
    ~LinuxRelayMachines() override;

    // The hosts the Relay lists now; a host no longer listed loses its link.
    void setListed(const QList<LinuxRelayListing>& listed);
    // Signed in with the Relay entitlement: the links run. Otherwise they stop.
    void setEnabled(bool enabled);
    [[nodiscard]] bool enabled() const { return enabled_; }
    // The Relay backend of the machine with this ADR 0119 machine id; null for any other id.
    [[nodiscard]] remote::MachineBackend* backend(const QString& machineId) const;
    // Rewrites every Relay row from its link, for the pending count's sake.
    void publish();

  private:
    void ensure(const QString& relayId);
    void publish(const QString& relayId);
    [[nodiscard]] QString relayIdOf(const QString& machineId) const;

    QPointer<LinuxMachineModel> model_;
    LinkFactory links_;
    // Relay id -> link.
    QHash<QString, LinuxRelayLink*> relayLinks_;
    // Relay ids the user disconnected: Offline without an error.
    QSet<QString> stopped_;
    bool enabled_{false};
};

} // namespace ketplus
