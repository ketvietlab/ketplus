#include "KetToast.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/ToastAvoidanceOwner.h"

#include <QQuickItem>
#include <QQuickView>
#include <QTimer>
#include <QtMath>

namespace ketplus {
using toast_avoidance::ownerProperty;

void AgentWorkspaceView::setupToastAvoidance() {
    refreshToastAncestors();
    if (auto* root = view_->rootObject()) {
        auto* composer = root->findChild<QQuickItem*>(QStringLiteral("agentBottomComposer"));
        for (auto* item = composer; item; item = item->parentItem()) {
            connect(item, &QQuickItem::yChanged, this, &AgentWorkspaceView::updateToastAvoidance);
            connect(item, &QQuickItem::heightChanged, this,
                    &AgentWorkspaceView::updateToastAvoidance);
            connect(item, &QQuickItem::visibleChanged, this,
                    &AgentWorkspaceView::updateToastAvoidance);
        }
    }
    QTimer::singleShot(0, this, &AgentWorkspaceView::updateToastAvoidance);
}

void AgentWorkspaceView::refreshToastAncestors() {
    if (toastAvoidanceTeardown_)
        return;
    for (const auto& ancestor : toastAncestors_)
        if (ancestor)
            ancestor->removeEventFilter(this);
    toastAncestors_.clear();
    for (QWidget* ancestor = this; ancestor; ancestor = ancestor->parentWidget()) {
        ancestor->installEventFilter(this);
        toastAncestors_.append(ancestor);
    }
    updateToastAvoidance();
}

void AgentWorkspaceView::stopToastAvoidance() {
    toastAvoidanceTeardown_ = true;
    if (toastHost_ && toastHost_->property(ownerProperty).value<QObject*>() == this) {
        toastHost_->setProperty(ownerProperty, QVariant());
        ket::Toast::setBottomAvoidance(toastHost_, 0);
    }
    for (const auto& ancestor : toastAncestors_)
        if (ancestor)
            ancestor->removeEventFilter(this);
    toastAncestors_.clear();
}

void AgentWorkspaceView::updateToastAvoidance() {
    if (toastAvoidanceTeardown_)
        return;
    QWidget* host = window();
    if (toastHost_ != host) {
        if (toastHost_ && toastHost_->property(ownerProperty).value<QObject*>() == this) {
            toastHost_->setProperty(ownerProperty, QVariant());
            ket::Toast::setBottomAvoidance(toastHost_, 0);
        }
        toastHost_ = host;
    }
    auto* root = view_->rootObject();
    auto* composer =
        root ? root->findChild<QQuickItem*>(QStringLiteral("agentBottomComposer")) : nullptr;
    if (!isVisible() || !composer || !composer->isVisible()) {
        if (host->property(ownerProperty).value<QObject*>() == this) {
            host->setProperty(ownerProperty, QVariant());
            ket::Toast::setBottomAvoidance(host, 0);
        }
        return;
    }
    const QPointF sceneTop = composer->mapToScene(QPointF());
    const int top = container_->mapTo(host, QPoint()).y() + qFloor(sceneTop.y());
    host->setProperty(ownerProperty, QVariant::fromValue<QObject*>(this));
    ket::Toast::setBottomAvoidance(host, qMax(0, host->height() - top));
}

} // namespace ketplus
