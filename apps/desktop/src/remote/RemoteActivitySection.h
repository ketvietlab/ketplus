#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace ketplus::remote {
class DaemonClient;
}

namespace ketplus {

// The words the activity shows for one `remote.audit` entry (#86), kept apart from the widgets
// so they can be checked directly.
namespace remote_activity {
struct Outcome {
    QString text;
    // positive, warning or danger, as the view's badge tones.
    QString tone;
    // Why a request was refused; empty when it was done.
    QString reason;
};
[[nodiscard]] QString action(const QString& kind);
[[nodiscard]] Outcome outcome(const QString& resultCode);
// "Just now", "12 min ago", "3 hours ago", "Yesterday, 21:14", "Mon, 09:02", "28 Oct, 17:55".
[[nodiscard]] QString when(const QDateTime& recordedAt, const QDateTime& now);
// The conversation a request was about: its title, "Deleted conversation" when it is gone, or
// empty when the request named none (a refused new conversation).
[[nodiscard]] QString target(const QJsonObject& entry);
// The device's name from the paired devices, "Unknown device" for an id without one.
[[nodiscard]] QString device(const QJsonObject& entry, const QHash<QString, QString>& names);
} // namespace remote_activity

// Connections › Activity from paired devices: the newest requests a paired device made that
// changed this Mac, and a dialog with every recorded one, all devices or one.
class RemoteActivitySection final : public QWidget {
    Q_OBJECT

  public:
    static constexpr int shownRows = 5;
    // The most entries a dialog asks for; it says so when there may be older ones.
    static constexpr int dialogLimit = 500;

    explicit RemoteActivitySection(remote::DaemonClient* daemon, QWidget* parent = nullptr);
    // Names for device ids, from the paired devices list.
    void setDeviceNames(const QHash<QString, QString>& names);
    // Asks the daemon again, at most every 15 s unless forced: the view refreshes its status
    // every 2 s.
    void refresh(bool force = false);
    void clear();
    // A paired device's details and activity; `revoke` runs when its access is revoked there.
    void showDevice(const QString& deviceId, const QString& name, std::function<void()> revoke);

  private:
    friend class RemoteControlViewTest;
    void render(const QJsonArray& entries);
    void showAll();
    void runDialog(const QString& title, const QString& deviceId, const QString& status,
                   std::function<void()> revoke);
    void fillRows(QVBoxLayout* rows, const QJsonArray& entries, bool showDevice) const;

    remote::DaemonClient* daemon_{nullptr};
    QHash<QString, QString> names_;
    QJsonArray entries_;
    QVBoxLayout* rows_{nullptr};
    QLabel* empty_{nullptr};
    QPushButton* showAll_{nullptr};
    bool requestInFlight_{false};
    QElapsedTimer lastRefresh_;
};

} // namespace ketplus
