#pragma once

#include <QJsonObject>
#include <QWidget>

#include <functional>

class QAbstractButton;
class QLabel;

namespace ket {
class Avatar;
} // namespace ket

namespace ketplus {

// Settings → Account & devices as in the mock: who signed this Mac in to KetPlus Relay
// (ADR 0109), Manage connections and Sign out. Connections owns signing in and out; this page
// asks it to. The mock's relay usage meter is left out: the relay reports no usage.
class SettingsAccountPage final : public QWidget {
    Q_OBJECT

  public:
    // Answers with the daemon's remote.status, or an empty object when it is unavailable.
    using StatusReader = std::function<void(std::function<void(const QJsonObject&)>)>;
    explicit SettingsAccountPage(StatusReader reader = {}, QWidget* parent = nullptr);

    void setStatusReader(StatusReader reader);
    void refresh();

  signals:
    void manageRequested();
    void signOutRequested();

  protected:
    void showEvent(QShowEvent* event) override;

  private:
    void apply(const QJsonObject& status);

    StatusReader reader_;
    ket::Avatar* avatar_{nullptr};
    QLabel* name_{nullptr};
    QLabel* meta_{nullptr};
    QAbstractButton* manage_{nullptr};
    QAbstractButton* signOut_{nullptr};
};

} // namespace ketplus
