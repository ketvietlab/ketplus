#include "KetToast.h"
#include "agent/ToastAvoidanceOwner.h"
#include "machine/MachineWorkspaceView.h"

#include <QEvent>
#include <QPlainTextEdit>
#include <QTimer>

namespace ketplus {
using toast_avoidance::ownerProperty;

MachineWorkspaceView::~MachineWorkspaceView() {
    toastTeardown_ = true;
    clearToastAvoidance();
    for (const auto& ancestor : toastAncestors_)
        if (ancestor)
            ancestor->removeEventFilter(this);
}

void MachineWorkspaceView::clearToastAvoidance() {
    if (toastHost_ && toastHost_->property(ownerProperty).value<QObject*>() == this) {
        toastHost_->setProperty(ownerProperty, QVariant());
        ket::Toast::setBottomAvoidance(toastHost_, 0);
    }
}

void MachineWorkspaceView::setupToastAvoidance() {
    refreshToastAncestors();
    QTimer::singleShot(0, this, &MachineWorkspaceView::updateToastAvoidance);
}

void MachineWorkspaceView::refreshToastAncestors() {
    if (toastTeardown_)
        return;
    for (const auto& ancestor : toastAncestors_)
        if (ancestor)
            ancestor->removeEventFilter(this);
    toastAncestors_.clear();
    for (QWidget* ancestor = composer_; ancestor; ancestor = ancestor->parentWidget()) {
        ancestor->installEventFilter(this);
        toastAncestors_.append(ancestor);
    }
    updateToastAvoidance();
}

bool MachineWorkspaceView::eventFilter(QObject* watched, QEvent* event) {
    if (!toastTeardown_) {
        if (event->type() == QEvent::ParentChange)
            refreshToastAncestors();
        if (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
            event->type() == QEvent::Show || event->type() == QEvent::Hide)
            updateToastAvoidance();
    }
    return QWidget::eventFilter(watched, event);
}

void MachineWorkspaceView::updateToastAvoidance() {
    if (toastTeardown_)
        return;
    QWidget* host = window();
    if (toastHost_ != host) {
        clearToastAvoidance();
        toastHost_ = host;
    }
    if (!isVisible() || !composer_->isVisible()) {
        clearToastAvoidance();
        return;
    }
    const int top = composer_->mapTo(host, QPoint()).y();
    host->setProperty(ownerProperty, QVariant::fromValue<QObject*>(this));
    ket::Toast::setBottomAvoidance(host, qMax(0, host->height() - top));
}
} // namespace ketplus
