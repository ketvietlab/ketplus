#pragma once

#include <QJsonObject>
#include <QPointer>
#include <QWidget>

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

  signals:
    void machinesChanged(const QStringList& ids, const QStringList& names);

  private:
    friend class RemoteControlViewTest;
    void render();
    void signIn();
    void startPairing();
    void refreshMachines();
    void setNotice(const QString& message, bool error = false);

    remote::ControlV2Client* client_;
    // The open pairing card is a child of the window, so the section deletes it itself.
    QPointer<ket::Dialog> pairingDialog_;
    QLabel* count_;
    QLabel* notice_;
    QVBoxLayout* rows_;
    QStringList machineIds_;
    bool loading_{false};
    bool pairingEntryShown_{false};
};

} // namespace ketplus
