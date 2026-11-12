#pragma once

#include <QJsonObject>
#include <QPointer>
#include <QWidget>

#include "remote/LinuxMachineModel.h"
#include "remote/LinuxRelayMachines.h"
#include "remote/LinuxSshMachines.h"

class QLabel;
class QAbstractButton;
class QTimer;
class QVBoxLayout;
namespace ket {
class Dialog;
}

namespace ketplus::remote {
class ControlV2Client;
class MachineBackend;
} // namespace ketplus::remote

namespace ketplus {

class LinuxMachinesSection final : public QWidget {
    Q_OBJECT
  public:
    explicit LinuxMachinesSection(QWidget* parent = nullptr);
    ~LinuxMachinesSection() override;
    void activate();
    void beginPairing();
    // Shows the section's own Connect button, for when the page's Add menu is not shown.
    void setPairingEntryShown(bool shown);
    [[nodiscard]] QStringList machineIds() const;
    // The machines the section shows. It owns its own model until another is set; the Relay's
    // trusted machines are written into whichever model it has.
    [[nodiscard]] LinuxMachineModel* model() const { return model_; }
    void setModel(LinuxMachineModel* model);
    // Attaches the SSH backend to the current model: the saved SSH machines, the SSH mode of the
    // connect card, and Connect, Disconnect and Remove. Production uses the defaults; tests
    // pass a temporary store and fake links.
    void attachSshBackend(QString storePath = remote::ssh::SshMachineStore::defaultPath(),
                          LinuxSshMachines::LinkFactory links = LinuxSshMachines::systemLinks());
    // Attaches the Relay backend: one link per host the Relay lists, running while the account
    // is entitled to the Relay. Production uses RelayMachineBackend over the section's client;
    // tests pass fake links.
    void attachRelayBackend(LinuxRelayMachines::LinkFactory links = {});
    // The ADR 0119 backend of a machine, for the composition the machine picker hands it to;
    // null while no backend carries it.
    [[nodiscard]] remote::MachineBackend* machineBackend(const QString& id) const;
    // The machine picker chose `id`. The seam T-5's composition binds to: until it does,
    // choosing a Linux machine only opens Connections.
    void selectMachine(const QString& id);

  signals:
    // The machines for the activity bar's picker: their ids, and labels with the same route
    // and state as the rows.
    void machinesChanged(const QStringList& ids, const QStringList& names);
    // A Linux machine was chosen in the picker, with its backend (null while none carries it).
    void machineSelected(const QString& id, ketplus::remote::MachineBackend* backend);

  private:
    friend class RemoteControlViewTest;
    friend class LinuxMachinesSectionTest;
    void render();
    void signIn();
    void refreshMachines();
    void setNotice(const QString& message, bool error = false);
    void openConnectCard(const QString& sshHost);
    void syncEntitlement();

    remote::ControlV2Client* client_;
    LinuxMachineModel* model_{nullptr};
    LinuxSshMachines* ssh_{nullptr};
    LinuxRelayMachines* relay_{nullptr};
    // Refreshes the Relay listing while the section is shown, so host presence stays current.
    QTimer* presence_;
    // The open connect card is a child of the window, so the section deletes it itself.
    QPointer<ket::Dialog> connectDialog_;
    QLabel* count_;
    QLabel* notice_;
    QVBoxLayout* rows_;
    bool loading_{false};
    bool pairingEntryShown_{false};
};

} // namespace ketplus
