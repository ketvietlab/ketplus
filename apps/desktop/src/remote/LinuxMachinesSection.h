#pragma once

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QAbstractButton;
class QVBoxLayout;

namespace ketplus::remote {
class ControlV2Client;
}

namespace ketplus {

class LinuxMachinesSection final : public QWidget {
    Q_OBJECT
  public:
    explicit LinuxMachinesSection(QWidget* parent = nullptr);
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
    QLabel* count_;
    QLabel* notice_;
    QVBoxLayout* rows_;
    QStringList machineIds_;
    bool loading_{false};
    bool pairingEntryShown_{false};
};

} // namespace ketplus
