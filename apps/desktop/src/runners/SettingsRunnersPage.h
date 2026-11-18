#pragma once

#include "KetDialog.h"
#include "runners/RunnerListModel.h"
#include "runners/RunnerPairingDialog.h"
#include "runners/RunnerRowWidget.h"
#include "runners/RunnersRequester.h"

#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace ket {
class Banner;
class Button;
class EmptyState;
} // namespace ket

namespace ketplus {

// Settings → Runners: the runners of the KetPlus account, how each relates to this computer,
// adding one with a pairing token, and pairing, disabling, enabling, unpairing or removing
// one. Every request goes through the injected requester; the page refreshes when shown,
// after each action, on runner.pairing.changed and on its Refresh button, never on a timer.
class SettingsRunnersPage final : public QWidget {
    Q_OBJECT

  public:
    explicit SettingsRunnersPage(runners::Requester requester = {}, QWidget* parent = nullptr);
    ~SettingsRunnersPage() override;

    void refresh();
    // A runner.pairing.changed event from the daemon.
    void pairingChanged(const QJsonObject& payload);

    [[nodiscard]] const runners::RunnerList& list() const { return list_; }
    [[nodiscard]] QList<runners::RunnerRowWidget*> rows() const;
    [[nodiscard]] runners::RunnerRowWidget* row(const QString& runnerId) const;
    [[nodiscard]] runners::RunnerPairingDialog* pairingDialog() const { return pairing_; }
    [[nodiscard]] ket::Dialog* confirmDialog() const { return confirm_; }
    [[nodiscard]] ket::EmptyState* emptyState() const { return empty_; }
    [[nodiscard]] ket::Button* addRunnerButton() const { return add_; }
    [[nodiscard]] ket::Button* refreshButton() const { return refresh_; }
    // The error shown above the list, or empty.
    [[nodiscard]] QString error() const;
    [[nodiscard]] QString note() const;

  signals:
    // The user asked to open Account & devices to sign in.
    void accountRequested();

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void receiveList(const QJsonObject& result, const QString& error);
    void receiveStatus(const runners::RunnerList& list, const QJsonObject& status,
                       const QString& error);
    void showList(const runners::RunnerList& list);
    void showUnavailable(runners::Unavailable unavailable);
    void showError(const QString& message);
    void onAction(runners::RowAction action, const QString& runnerId);
    void confirm(runners::RowAction action, const QString& runnerId);
    void send(const QString& type, const QString& runnerId);
    void openPairing(const QString& runnerId, const QString& runnerName);
    void closeDialogs();

    runners::Requester requester_;
    runners::RunnerList list_;
    QSet<QString> busy_;
    QPointer<runners::RunnerPairingDialog> pairing_;
    QPointer<ket::Dialog> confirm_;

    ket::Button* add_;
    ket::Button* refresh_;
    ket::Banner* error_;
    ket::Banner* pinWarning_;
    ket::EmptyState* empty_;
    QWidget* listBox_;
    QVBoxLayout* listLayout_;
    QLabel* note_;

    enum class EmptyAction { None, Account, Add, Refresh };
    EmptyAction emptyAction_{EmptyAction::None};
    // A failed row action, shown again once the refresh after it is in.
    QString actionError_;
    bool listing_{false};
    bool queued_{false};
};

} // namespace ketplus
