#pragma once

#include <QJsonObject>
#include <QPointer>
#include <QWidget>

#include "remote/LinuxMachineModel.h"

class QLabel;
class QAbstractButton;
class QVBoxLayout;
namespace ket {
class Dialog;
}

namespace ketplus::remote {
class ControlV2Client;
}

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

  signals:
    void machinesChanged(const QStringList& ids, const QStringList& names);

  private:
    friend class RemoteControlViewTest;
    friend class LinuxMachinesSectionTest;
    void render();
    void signIn();
    void refreshMachines();
    void setNotice(const QString& message, bool error = false);

    remote::ControlV2Client* client_;
    LinuxMachineModel* model_{nullptr};
    // The open connect card is a child of the window, so the section deletes it itself.
    QPointer<ket::Dialog> connectDialog_;
    QLabel* count_;
    QLabel* notice_;
    QVBoxLayout* rows_;
    bool loading_{false};
    bool pairingEntryShown_{false};
};

} // namespace ketplus
