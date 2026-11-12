#include "KetToast.h"
#include "agent/AgentWorkspaceView.h"

#include <QQuickItem>
#include <QQuickView>
#include <QTimer>
#include <QtMath>

namespace ketplus {
namespace {
constexpr char ownerProperty[] = "_ketplus_toast_composer_owner";
}

void AgentWorkspaceView::setupToastAvoidance() {
    for (QWidget* ancestor = this; ancestor; ancestor = ancestor->parentWidget())
        ancestor->installEventFilter(this);
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
    if (window() != this)
        connect(this, &QObject::destroyed, window(), [host = window(), owner = this] {
            if (host->property(ownerProperty).value<QObject*>() == owner) {
                host->setProperty(ownerProperty, QVariant());
                ket::Toast::setBottomAvoidance(host, 0);
            }
        });
    QTimer::singleShot(0, this, &AgentWorkspaceView::updateToastAvoidance);
}

void AgentWorkspaceView::updateToastAvoidance() {
    QWidget* host = window();
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
