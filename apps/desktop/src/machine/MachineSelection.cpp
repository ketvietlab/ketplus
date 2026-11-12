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

remote::MachineBackend* MachineSelection::backend() const {
    if (auto* selected = backendFor(selected_); selected != nullptr)
        return selected;
    return backendFor(localId);
}

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

void MachineSelection::removeBackend(const QString& machineId) {
    if (machineId == localId || !backends_.remove(machineId))
        return;
    emit backendsChanged();
    if (machineId == selected_) {
        selected_ = localId;
        emit selectionChanged(selected_, backend());
    }
}

void MachineSelection::select(const QString& machineId) {
    if (machineId == selected_ || backendFor(machineId) == nullptr)
        return;
    selected_ = machineId;
    emit selectionChanged(selected_, backend());
}

} // namespace ketplus
