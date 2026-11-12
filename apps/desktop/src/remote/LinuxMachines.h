#pragma once

#include "remote/LinuxMachineModel.h"
#include "remote/LinuxRelayMachines.h"
#include "remote/LinuxSshMachines.h"

#include <QList>
#include <QObject>
#include <QPointer>

class QTimer;
class MachineEndToEndTest;
class MachineWindowTest;

namespace ketplus::remote {
class ControlV2Client;
class MachineBackend;
} // namespace ketplus::remote

namespace ketplus {

class LinuxMachinesSection;

// The app's one set of Linux machines (ADR 0119, docs/ssh-transport.md "Connections"): the
// account client, the model, one SSH backend per saved machine and one Relay link (and so one
// outbox writer) per Relay host. main() owns it and every window's Connections section, picker
// and MachineSelection read from it, so opening or closing a window neither starts nor drops a
// connection; destroying it (quitting) closes them. A section without a shared set owns one.
class LinuxMachines final : public QObject {
    Q_OBJECT

  public:
    explicit LinuxMachines(QObject* parent = nullptr);
    ~LinuxMachines() override;

    [[nodiscard]] remote::ControlV2Client* client() const { return client_; }
    [[nodiscard]] LinuxMachineModel* model() const { return model_; }
    // Attaches the SSH backend: the saved SSH machines, the SSH mode of the connect card, and
    // Connect, Disconnect and Remove. Production uses the defaults; tests pass a temporary store
    // and fake links.
    void attachSshBackend(QString storePath = remote::ssh::SshMachineStore::defaultPath(),
                          LinuxSshMachines::LinkFactory links = LinuxSshMachines::systemLinks());
    // Attaches the Relay backend: one link per host the Relay lists, running while the account
    // is entitled to the Relay. Production uses RelayMachineBackend over the client; tests pass
    // fake links.
    void attachRelayBackend(LinuxRelayMachines::LinkFactory links = {});
    [[nodiscard]] bool sshAttached() const { return ssh_ != nullptr; }
    [[nodiscard]] bool relayAttached() const { return relay_ != nullptr; }
    // The ADR 0119 backend of a machine: the route its row shows, SSH or the Relay while it is
    // the fallback; null while no backend carries it.
    [[nodiscard]] remote::MachineBackend* machineBackend(const QString& id) const;

    // Connects the saved SSH machines the user did not close, follows the session's Relay
    // entitlement and refreshes the Relay listing when signed in. main() calls it at launch, and
    // a section each time Connections is shown.
    void start();
    void refreshMachines();
    void signIn();
    [[nodiscard]] bool loading() const { return loading_; }

    // A window's section shows these machines. The connect card a prompt opens (after Forget
    // old key) appears once, in the section Connections was last shown in.
    void addSection(LinuxMachinesSection* section);
    void removeSection(LinuxMachinesSection* section);
    void setPromptSection(LinuxMachinesSection* section);
    // Emits machinesChanged for the machines as they are now.
    void publishMachines();

  signals:
    // The machines for the activity bar's picker: their ids, and labels with the same route
    // and state as the rows.
    void machinesChanged(const QStringList& ids, const QStringList& names);
    // The account, the entitlement or the loading state changed: sections render again.
    void changed();
    void notice(const QString& message, bool error);

  private:
    friend class RemoteControlViewTest;
    friend class LinuxMachinesSectionTest;
    friend class ::MachineEndToEndTest;
    friend class ::MachineWindowTest;
    void syncEntitlement();
    [[nodiscard]] LinuxMachinesSection* promptSection() const;
    [[nodiscard]] bool shown() const;

    remote::ControlV2Client* client_;
    LinuxMachineModel* model_;
    LinuxSshMachines* ssh_{nullptr};
    LinuxRelayMachines* relay_{nullptr};
    // Refreshes the Relay listing while a section is shown, so host presence stays current.
    QTimer* presence_;
    QList<QPointer<LinuxMachinesSection>> sections_;
    QPointer<LinuxMachinesSection> promptSection_;
    bool loading_{false};
};

} // namespace ketplus
