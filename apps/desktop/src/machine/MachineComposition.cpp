#include "machine/MachineComposition.h"

#include "DaemonMachineBackend.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineSelection.h"
#include "machine/MachineSidebar.h"
#include "machine/MachineWorkspaceView.h"

namespace ketplus {

MachineComposition::MachineComposition(remote::DaemonClient* localDaemon, QObject* parent)
    : QObject(parent), local_(new remote::DaemonMachineBackend(localDaemon, this)),
      selection_(new MachineSelection(local_, this)), catalog_(new MachineCatalog(this)) {
    connect(selection_, &MachineSelection::selectionChanged, this,
            [this](const QString& machineId, remote::MachineBackend* backend) {
                const bool remote = machineId != QLatin1String("local");
                // This Mac keeps its own sidebar and workspace; the catalog binds only to a
                // remote machine, so nothing it lists can mix with the local view.
                catalog_->setBackend(remote ? backend : nullptr);
                emit machineChanged(machineId, remote);
            });
    connect(catalog_, &MachineCatalog::changed, this, [this] {
        if (remoteSelected())
            emit sidebarChanged();
    });
}

MachineComposition::~MachineComposition() {
    // The view reads the catalog; it goes first.
    delete view_.data();
}

bool MachineComposition::remoteSelected() const {
    return selection_->machineId() != QLatin1String("local");
}

MachineWorkspaceView* MachineComposition::view(QWidget* parent) {
    if (view_ == nullptr) {
        view_ = new MachineWorkspaceView(catalog_, parent);
        connect(view_, &MachineWorkspaceView::conversationOpened, this,
                &MachineComposition::sidebarChanged);
    }
    return view_;
}

MachineWorkspaceView* MachineComposition::existingView() const { return view_; }

bool MachineComposition::shown() const { return view_ != nullptr && !view_->isHidden(); }

QVector<SidebarRepository> MachineComposition::sidebarRepositories() const {
    if (!remoteSelected())
        return {};
    return machineSidebarRepositories(*catalog_,
                                      view_ != nullptr ? view_->openSessionId() : QString{});
}

} // namespace ketplus
