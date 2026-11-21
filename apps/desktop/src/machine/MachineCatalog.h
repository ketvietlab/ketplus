#pragma once

#include "MachineBackend.h"
#include "MachineRows.h"

#include <QObject>
#include <QPointer>

namespace ketplus {

// ADR 0119: the repositories, worktrees and conversations of the selected machine, read through
// its MachineBackend. Switching machines replaces the lists, never merges them; answers from a
// machine no longer bound are dropped. Connection states follow ADR 0119 "Connection states":
// Connecting and Offline show no list, Reconnecting keeps the last lists read-only, and a return
// to Connected lists again on its own. catalogChanged() lists again, which fills in worktrees the
// route was still verifying.
class MachineCatalog final : public QObject {
    Q_OBJECT

  public:
    using State = remote::MachineBackend::ConnectionState;

    explicit MachineCatalog(QObject* parent = nullptr);

    void setBackend(remote::MachineBackend* backend);
    [[nodiscard]] remote::MachineBackend* backend() const { return backend_.data(); }
    [[nodiscard]] QString machineId() const;

    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] QString reason() const { return reason_; }
    // Lists are shown read-only: the machine is Reconnecting.
    [[nodiscard]] bool readOnly() const { return state_ == State::Reconnecting; }
    // A listing is on its way and nothing has been listed for this machine yet.
    [[nodiscard]] bool loading() const { return loading_; }
    // Why a listing failed, for the sidebar; empty after a listing succeeds.
    [[nodiscard]] QString error() const { return error_; }

    [[nodiscard]] const QVector<remote::MachineRepository>& repositories() const {
        return repositories_;
    }
    [[nodiscard]] const QVector<remote::MachineWorktree>& pendingWorktrees() const {
        return pending_;
    }
    [[nodiscard]] const QVector<remote::MachineConversation>& conversations() const {
        return conversations_;
    }
    // The repository a conversation belongs to: its own repository id, or the repository that
    // lists its worktree. Empty when neither is known.
    [[nodiscard]] QString repositoryOf(const remote::MachineConversation& conversation) const;
    [[nodiscard]] remote::MachineConversation conversation(const QString& sessionId) const;

  public slots:
    void refresh();

  signals:
    void changed();

  private:
    void setState(State state, const QString& reason);
    void clear();

    QPointer<remote::MachineBackend> backend_;
    QVector<QMetaObject::Connection> connections_;
    State state_{State::Offline};
    QString reason_;
    QString error_;
    bool loading_{false};
    bool refreshQueued_{false};
    quint64 binding_{0};
    QVector<remote::MachineRepository> repositories_;
    QVector<remote::MachineWorktree> pending_;
    QVector<remote::MachineConversation> conversations_;
};

} // namespace ketplus
