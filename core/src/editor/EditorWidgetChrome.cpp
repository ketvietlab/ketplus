#include "editor/EditorWidget.h"

#include "KetAppearance.h"
#include "KetChrome.h"

#include <QDir>
#include <QResizeEvent>

namespace ketplus {

void EditorWidget::setBreadcrumbPath(const QString& path) {
    if (!breadcrumb_) {
        breadcrumb_ = new ket::Breadcrumb(this);
        breadcrumb_->setObjectName(QStringLiteral("editorBreadcrumb"));
        connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this,
                [this] { updateBreadcrumbGeometry(); });
    }
    breadcrumb_->setPath(QDir::fromNativeSeparators(path).split('/', Qt::SkipEmptyParts));
    breadcrumb_->setToolTip(path);
    breadcrumb_->setVisible(!path.isEmpty());
    updateBreadcrumbGeometry();
}

void EditorWidget::updateBreadcrumbGeometry() {
    const int height = breadcrumb_ && !breadcrumb_->isHidden() ? breadcrumb_->height() : 0;
    if (viewportMargins().top() != height)
        setViewportMargins(0, height, 0, 0);
    if (breadcrumb_)
        breadcrumb_->setGeometry(contentsRect().left(), contentsRect().top(),
                                 contentsRect().width(), height);
}

void EditorWidget::resizeEvent(QResizeEvent* event) {
    ScintillaEditBase::resizeEvent(event);
    updateBreadcrumbGeometry();
}

} // namespace ketplus
