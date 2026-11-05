#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class QEvent;
class QVBoxLayout;
class QDialog;
namespace ketplus {
class LinuxMachinesSection;
}

namespace ketplus::remote {
class DaemonClient;
}

namespace ketplus {

class RemoteControlView final : public QWidget {
    Q_OBJECT

  public:
    explicit RemoteControlView(QWidget* parent = nullptr);
    void activate();
    void applyDesignSystemStyle();

  signals:
    void machinesChanged(const QStringList& ids, const QStringList& names);

  private:
    QWidget* createSignInPage();
    QWidget* createControlPage();
    void refreshStatus();
    void applyStatus(const QJsonObject& status);
    void refreshDevices();
    void renderDevices(const QJsonArray& devices);
    void signIn();
    void cancelSignIn();
    void resetSignInButton();
    void signOut();
    void setSignInNotice(const QString& text, bool failure = false);
    void createPairing();
    void refreshPairing();
    void approvePairing();
    void showDeviceDetails(const QString& deviceId, const QString& displayName);
    void revokeDevice(const QString& deviceId);
    void closePairing();
    void setNotice(const QString& text, bool failure = false);
    void setBusy(bool busy);
    void showPairing(const QJsonObject& ticket);

  protected:
    void changeEvent(QEvent* event) override;

    remote::DaemonClient* daemon_{nullptr};
    QTimer* refreshTimer_{nullptr};
    QStackedWidget* pages_{nullptr};
    QPushButton* signInButton_{nullptr};
    QPushButton* signInPromptButton_{nullptr};
    QWidget* unlinkedContent_{nullptr};
    QWidget* linkedContent_{nullptr};
    LinuxMachinesSection* linuxMachines_{nullptr};
    QLabel* notice_{nullptr};
    QLabel* welcomeNotice_{nullptr};
    QLabel* signInNotice_{nullptr};
    QVBoxLayout* devicesLayout_{nullptr};
    QLabel* deviceCount_{nullptr};
    QLabel* devicesEmpty_{nullptr};
    QLabel* overviewDescription_{nullptr};
    QLabel* overviewStatus_{nullptr};
    QPushButton* pairButton_{nullptr};
    QPushButton* signOutButton_{nullptr};
    QDialog* pairingDialog_{nullptr};
    QDialog* signInDialog_{nullptr};
    QWidget* pageHeader_{nullptr};
    QWidget* pairingPanel_{nullptr};
    QWidget* pairingScanSection_{nullptr};
    QLabel* pairingDialogTitle_{nullptr};
    QLabel* qrCode_{nullptr};
    QLineEdit* pairingCode_{nullptr};
    QLabel* pairingState_{nullptr};
    QLabel* pairingHelp_{nullptr};
    QLabel* pairingTimer_{nullptr};
    QLabel* pairingSteps_{nullptr};
    QLabel* pairingConfirmationCode_{nullptr};
    QLabel* pairingNotice_{nullptr};
    QLabel* pairingDemoNote_{nullptr};
    QWidget* pairingDemoNoteHost_{nullptr};
    QLabel* deviceClaim_{nullptr};
    QPushButton* pairingCancelButton_{nullptr};
    QPushButton* approveButton_{nullptr};
    QString ticketId_;
    quint64 pairingEpoch_{0};
    bool requestInFlight_{false};
    bool devicesRequestInFlight_{false};
    bool devicesLoaded_{false};
    bool configured_{false};
    bool offline_{false};
    bool demoMode_{false};
    // The browser is open on the sign-in page; remote.status says when it finishes.
    bool signInWaiting_{false};
    bool applyingDesignSystemStyle_{false};
};

} // namespace ketplus
