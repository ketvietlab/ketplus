#include "machine/MachineSelection.h"

#include "MachineBackend.h"

#include <algorithm>

namespace ketplus {

namespace {
const QString localId = QStringLiteral("local");
}

MachineSelection::MachineSelection(remote::MachineBackend* local, QObject* parent)
    : QObject(parent) {
    if (local != nullptr)
        backends_.insert(localId, local);
}

remote::MachineBackend* MachineSelection::backend() const { return backendFor(selected_); }

remote::MachineBackend* MachineSelection::backendFor(const QString& machineId) const {
    return backends_.value(machineId).data();
}

QStringList MachineSelection::machineIds() const {
    QStringList ids;
    for (auto it = backends_.cbegin(); it != backends_.cend(); ++it)
        if (!it.value().isNull())
            ids.append(it.key());
    std::sort(ids.begin(), ids.end(), [](const QString& left, const QString& right) {
        return (left == localId) != (right == localId) ? left == localId : left < right;
    });
    return ids;
}

void MachineSelection::setBackend(const QString& machineId, remote::MachineBackend* backend) {
    if (machineId.isEmpty() || backend == nullptr || backendFor(machineId) == backend)
        return;
    backends_.insert(machineId, backend);
    connect(backend, &QObject::destroyed, this, [this, machineId] {
        // A replaced backend's destruction must not forget its successor.
        if (backends_.value(machineId).isNull())
            removeBackend(machineId);
    });
    emit backendsChanged();
    if (machineId == selected_)
        emit selectionChanged(selected_, backend);
}

void MachineSelection::syncBackends(
    const QStringList& machineIds,
    const std::function<remote::MachineBackend*(const QString&)>& backendFor) {
    for (const QString& id : machineIds)
        if (id != localId)
            if (auto* backend = backendFor(id); backend != nullptr)
                setBackend(id, backend);
    for (const QString& id : this->machineIds())
        if (id != localId && (!machineIds.contains(id) || backendFor(id) == nullptr))
            removeBackend(id);
    // A selected machine Connections no longer lists is gone: the window returns to This Mac.
    if (selected_ != localId && !machineIds.contains(selected_))
        select(localId);
}

void MachineSelection::removeBackend(const QString& machineId) {
    if (machineId == localId || !backends_.remove(machineId))
        return;
    emit backendsChanged();
    // Still selected, now without a route: the views show it as not connected and never fall
    // back to This Mac (ADR 0119). Connections removing the machine selects This Mac.
    if (machineId == selected_)
        emit selectionChanged(selected_, nullptr);
}

void MachineSelection::select(const QString& machineId) {
    if (machineId.isEmpty() || machineId == selected_)
        return;
    // A machine without a backend yet stays selected; its backend arrives through setBackend().
    selected_ = machineId;
    emit selectionChanged(selected_, backend());
}

} // namespace ketplus
