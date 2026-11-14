#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

namespace ketplus::remote {
class MachineBackend;
}

namespace ketplus {

// ADR 0119 "Binding to the selected machine": the window's one selected machine and the
// MachineBackend of every machine the window knows. Connections registers a backend per machine
// (SSH preferred over the Relay for the same fingerprint); the machine picker selects one. The
// sidebar, worktree picker and Agent Workspace read the selection, never a transport. Backends
// stay owned by whoever registered them; one destroyed is forgotten.
class MachineSelection final : public QObject {
    Q_OBJECT

  public:
    explicit MachineSelection(remote::MachineBackend* local, QObject* parent = nullptr);

    [[nodiscard]] QString machineId() const { return selected_; }
    // The selected machine's backend; nullptr while a picked machine has none yet (it is then
    // shown as not connected, never as This Mac).
    [[nodiscard]] remote::MachineBackend* backend() const;
    // The backend of a machine a conversation was opened on, even after the picker changed;
    // nullptr when the window no longer knows that machine.
    [[nodiscard]] remote::MachineBackend* backendFor(const QString& machineId) const;
    [[nodiscard]] QStringList machineIds() const;

    // Registers or replaces a machine's backend; a replacement for the selected machine emits
    // selectionChanged() so every view rebinds (for example SSH added for a Relay host).
    void setBackend(const QString& machineId, remote::MachineBackend* backend);
    // Connections' machine list changed: every listed id with a backend is registered or
    // replaced, and every other remote machine is forgotten; a selected machine that is no
    // longer listed returns the window to This Mac. Call it from the slot that sees the change,
    // before a removed backend is deleted.
    void syncBackends(const QStringList& machineIds,
                      const std::function<remote::MachineBackend*(const QString&)>& backendFor);
    // Forgets a machine's backend; a selected machine stays selected without one.
    void removeBackend(const QString& machineId);
    // Selects a machine, with or without a backend yet; the current one changes nothing.
    void select(const QString& machineId);

  signals:
    void selectionChanged(const QString& machineId, ketplus::remote::MachineBackend* backend);
    void backendsChanged();

  private:
    QHash<QString, QPointer<remote::MachineBackend>> backends_;
    QString selected_{QStringLiteral("local")};
};

} // namespace ketplus
