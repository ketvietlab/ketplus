#pragma once

#include <QJsonObject>
#include <QPointer>
#include <QWidget>

#include "remote/LinuxMachineModel.h"
#include "remote/LinuxMachines.h"
#include "remote/LinuxRelayMachines.h"
#include "remote/LinuxSshMachines.h"

class QLabel;
class QAbstractButton;
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
    // The Linux machines the section shows: its own set until the app's shared one is set
    // (main() shares one set with every window). Setting it emits machinesChanged at once.
    // The section makes its own set only when it is used without a shared one.
    [[nodiscard]] LinuxMachines* machines() const;
    void setMachines(LinuxMachines* machines);
    // The set's model; an empty one once the app's set is gone (quitting).
    [[nodiscard]] LinuxMachineModel* model() const;
    // Shows `model` instead of the set's own (LinuxMachines::setModel), for tests that drive a
    // model and its controllers themselves.
    void setModel(LinuxMachineModel* model);
    // The set's backends (LinuxMachines); a shared set is attached once, by main().
    void attachSshBackend(QString storePath = remote::ssh::SshMachineStore::defaultPath(),
                          LinuxSshMachines::LinkFactory links = LinuxSshMachines::systemLinks());
    void attachRelayBackend(LinuxRelayMachines::LinkFactory links = {});
    // The ADR 0119 backend of a machine, which the window's MachineSelection registers for each
    // id in machinesChanged; null while no backend carries it.
    [[nodiscard]] remote::MachineBackend* machineBackend(const QString& id) const;

  signals:
    // The machines for the activity bar's picker: their ids, and labels with the same route
    // and state as the rows.
    void machinesChanged(const QStringList& ids, const QStringList& names);

  private:
    friend class RemoteControlViewTest;
    friend class LinuxMachinesSectionTest;
    friend class LinuxMachines;
    void render();
    void setNotice(const QString& message, bool error = false);
    void openConnectCard(const QString& sshHost);
    void connectMachines();
    void connectModel();
    void showEvent(QShowEvent* event) override;

    QPointer<LinuxMachines> machines_;
    QPointer<LinuxMachineModel> shownModel_;
    // What model() returns after the shared set is gone.
    LinuxMachineModel* emptyModel_{nullptr};
    // The set the section made for itself, deleted when a shared one replaces it.
    bool ownsMachines_{false};
    // A shared set was set: once it is gone (the app is quitting) none is made in its place.
    bool shared_{false};
    // The open connect card is a child of the window, so the section deletes it itself.
    QPointer<ket::Dialog> connectDialog_;
    QLabel* count_;
    QLabel* notice_;
    QVBoxLayout* rows_;
    bool pairingEntryShown_{false};
};

} // namespace ketplus
