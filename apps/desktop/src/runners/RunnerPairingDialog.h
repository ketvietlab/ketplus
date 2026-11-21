#pragma once

#include "KetDialog.h"
#include "runners/RunnerPairingModel.h"
#include "runners/RunnersRequester.h"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <optional>

class QLabel;
class QWidget;

namespace ket {
class Button;
class TextField;
} // namespace ket

namespace ketplus::runners {

// Adds a runner, or pairs a listed one again: creates a token, shows it masked with the
// command to run on the node, then shows the runner and the confirmation code once the node
// claimed the token. Only a click on Approve approves. The token lives only in the masked
// field and is cleared whenever the flow ends, expires or the dialog closes. Closing before
// the flow ended rejects the pairing once.
class RunnerPairingDialog final : public QObject {
    Q_OBJECT

  public:
    // An empty `runnerId` adds a new runner; otherwise the listed runner is paired again.
    RunnerPairingDialog(Requester requester, QWidget* anchor, const QString& runnerId,
                        const QString& runnerName, QObject* parent = nullptr);
    ~RunnerPairingDialog() override;

    // Asks for a token: runner.pairing.create, once.
    void start();
    // A runner.pairing.changed event: reads the pairing once when it is this dialog's.
    void pairingChanged(const QJsonObject& payload);
    // As Esc or the close button.
    void close();

    [[nodiscard]] ket::Dialog* dialog() const { return dialog_; }
    [[nodiscard]] ket::TextField* tokenField() const { return token_; }
    [[nodiscard]] ket::Button* approveButton() const { return approve_; }
    [[nodiscard]] ket::Button* rejectButton() const { return reject_; }
    [[nodiscard]] ket::Button* checkButton() const { return check_; }
    [[nodiscard]] ket::Button* copyTokenButton() const { return copyToken_; }
    [[nodiscard]] ket::Button* copyCommandButton() const { return copyCommand_; }
    [[nodiscard]] QString message() const;
    [[nodiscard]] QString error() const;
    [[nodiscard]] std::optional<PairingView> pairing() const { return view_; }
    [[nodiscard]] bool isFinished() const { return finished_; }

  signals:
    // The dialog closed; `paired` when the runner is now trusted. The owner refreshes its
    // list and deletes this object.
    void closed(bool paired);

  private:
    void receiveCreate(const QJsonObject& result, const QString& error);
    void receivePairing(const QJsonObject& result, const QString& error, bool fromApprove);
    void readStatus();
    void approveClicked();
    void rejectClicked();
    void onDialogFinished();
    void expire();
    void show(const PairingView& view);
    void showError(const QString& text);
    void clearToken();
    void sendReject();

    Requester requester_;
    QPointer<ket::Dialog> dialog_;
    QString runnerId_;
    bool newRunner_;
    std::optional<PairingView> view_;
    QTimer expiry_;

    QLabel* message_;
    QLabel* error_;
    QWidget* tokenBox_;
    ket::TextField* token_;
    QLabel* expires_;
    QLabel* command_;
    QWidget* runnerBox_;
    QLabel* runnerName_;
    QLabel* runnerDetail_;
    QLabel* fingerprint_;
    QLabel* code_;
    ket::Button* copyToken_;
    ket::Button* copyCommand_;
    ket::Button* check_;
    ket::Button* reject_;
    ket::Button* approve_;

    bool creating_{false};
    bool reading_{false};
    bool approving_{false};
    bool rejectSent_{false};
    bool finished_{false};
    bool mismatch_{false};
    bool closed_{false};
};

} // namespace ketplus::runners
