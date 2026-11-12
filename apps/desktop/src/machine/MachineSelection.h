#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

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
    // Never null while This Mac's backend lives: "local" when nothing else is selected.
    [[nodiscard]] remote::MachineBackend* backend() const;
    // The backend of a machine a conversation was opened on, even after the picker changed;
    // nullptr when the window no longer knows that machine.
    [[nodiscard]] remote::MachineBackend* backendFor(const QString& machineId) const;
    [[nodiscard]] QStringList machineIds() const;

    void setBackend(const QString& machineId, remote::MachineBackend* backend);
    // Forgets a machine; the selection returns to This Mac if it was selected.
    void removeBackend(const QString& machineId);
    // Selects a known machine; an unknown id or the current one changes nothing.
    void select(const QString& machineId);

  signals:
    void selectionChanged(const QString& machineId, ketplus::remote::MachineBackend* backend);
    void backendsChanged();

  private:
    QHash<QString, QPointer<remote::MachineBackend>> backends_;
    QString selected_{QStringLiteral("local")};
};

} // namespace ketplus
