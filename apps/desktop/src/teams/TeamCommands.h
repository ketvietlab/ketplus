#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QAbstractButton;
class QAction;
class QLabel;
class QTimer;
class QWidget;

namespace ketplus {

class TeamDirectory;

// A team command shows at once that it was taken, and it is sent once. Each command has a
// key naming what it changes (one decision, the team's pause, one task's owner). While a
// command is pending, every control bound to its key is disabled, the control that was
// activated keeps the kit's pressed look, and sending under the key again sends nothing.
// After stillWaitingMs without a reply the activated control and the key's status label say
// so, and the command stays pending: the reply decides. The state lives here and not in the
// widgets, so a page rebuilt meanwhile binds its new controls to the same pending command.
//
// Nothing here waits: send() returns as soon as the request is handed to the directory.
class TeamCommands final : public QObject {
    Q_OBJECT

  public:
    using Reply = std::function<void(const QJsonObject& result, const QString& error)>;
    static constexpr int stillWaitingMs = 10000;

    explicit TeamCommands(TeamDirectory* teams);

    // The key of `what` in one team, such as key("7", "decision/D-1").
    [[nodiscard]] static QString key(const QString& teamId, const QString& what);

    // Sends unless a command under `key` is pending, and returns whether it sent. `source`
    // names the control that was activated among those bound to the key, such as a decision's
    // choice; empty marks every bound control without a source of its own.
    bool send(const QString& key, const QString& source, const QString& type,
              const QJsonObject& payload, Reply reply = {});

    [[nodiscard]] bool isPending(const QString& key) const { return pending_.contains(key); }
    [[nodiscard]] bool isWaitingLong(const QString& key) const;

    // The control follows the key's pending state for as long as it lives. A control with
    // the pending command's source (or, without one, any control) shows it was activated.
    void bind(QAbstractButton* control, const QString& key, const QString& source = {});
    void bind(QAction* action, const QString& key);
    // The label beside the controls: empty while the command is on its way, saying it is
    // still waiting after stillWaitingMs, and showing the daemon's reason for a refusal. It
    // is the label bound when the answer comes, so one rebuilt meanwhile shows the reason.
    void bindStatus(QLabel* label, const QString& key);

    void setStillWaitingForTesting(int milliseconds) { stillWaiting_ = milliseconds; }

  signals:
    void pendingChanged(const QString& key);

  private:
    struct Pending final {
        QString source;
        bool waitingLong{false};
        QTimer* timer{nullptr};
    };
    struct Bound final {
        QPointer<QObject> control;
        QString key;
        QString source;
        // A label beside the controls rather than a control.
        bool status{false};
        // What the control was before the command, given back when it ends.
        bool enabled{true};
        QString text;
        bool applied{false};
        bool waitingShown{false};
        // Where Qt moved the keyboard focus when the control was disabled while it had it.
        QPointer<QWidget> focusMovedTo;
        bool hadFocus{false};
    };

    void finish(const QString& key, const QString& error);
    void apply(Bound& bound);
    void restore(Bound& bound);
    void applyAll(const QString& key);
    [[nodiscard]] bool activated(const Bound& bound) const;
    [[nodiscard]] bool hasStatus(const QString& key) const;
    void prune();

    TeamDirectory* teams_;
    QHash<QString, Pending> pending_;
    QList<Bound> bound_;
    int stillWaiting_{stillWaitingMs};
};

} // namespace ketplus
