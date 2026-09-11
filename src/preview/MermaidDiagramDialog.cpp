#include "preview/MermaidDiagramDialog.h"

#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsSvgItem>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <algorithm>

namespace ketplus {
namespace {

class MermaidDiagramView final : public QGraphicsView {
  public:
    MermaidDiagramView(const QString& svgPath, const ThemePalette& palette, QWidget* parent)
        : QGraphicsView(parent), scene_(new QGraphicsScene(this)) {
        if (QImageReader(svgPath).format() == "svg")
            diagram_ = new QGraphicsSvgItem(svgPath);
        else {
            QPixmap pixmap(svgPath);
            pixmap.setDevicePixelRatio(2);
            diagram_ = new QGraphicsPixmapItem(pixmap);
        }
        setScene(scene_);
        scene_->addItem(diagram_);
        diagram_->setCacheMode(QGraphicsItem::NoCache);
        scene_->setSceneRect(diagram_->boundingRect().adjusted(-32, -32, 32, 32));

        setObjectName(QStringLiteral("mermaidDiagramView"));
        setDragMode(QGraphicsView::ScrollHandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setResizeAnchor(QGraphicsView::AnchorViewCenter);
        setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                       QPainter::SmoothPixmapTransform);
        setStyleSheet(QStringLiteral("QGraphicsView { border: 1px solid %1; background: %2; }")
                          .arg(palette.border, palette.panelBackground));
    }

    void fitDiagram() {
        resetTransform();
        fitInView(diagram_->boundingRect().adjusted(-24, -24, 24, 24), Qt::KeepAspectRatio);
        currentScale_ = transform().m11();
    }

    void actualSize() {
        resetTransform();
        currentScale_ = 1.0;
        centerOn(diagram_);
    }

    void zoomIn() { zoomBy(1.25, viewport()->rect().center()); }

    void zoomOut() { zoomBy(0.8, viewport()->rect().center()); }

  protected:
    bool viewportEvent(QEvent* event) override {
        if (event->type() == QEvent::NativeGesture) {
            auto* gesture = static_cast<QNativeGestureEvent*>(event);
            if (gesture->gestureType() == Qt::ZoomNativeGesture) {
                zoomBy(std::exp(gesture->value()), gesture->position());
                gesture->accept();
                return true;
            }
        }
        return QGraphicsView::viewportEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override {
        const int delta =
            event->angleDelta().y() != 0 ? event->angleDelta().y() : event->pixelDelta().y() * 4;
        zoomBy(std::pow(1.0015, delta), event->position());
        event->accept();
    }

  private:
    void zoomBy(const double factor, const QPointF& viewportPosition) {
        constexpr double minimumScale = 0.08;
        constexpr double maximumScale = 24.0;
        const double nextScale = std::clamp(currentScale_ * factor, minimumScale, maximumScale);
        const double appliedFactor = nextScale / currentScale_;
        if (qFuzzyCompare(appliedFactor, 1.0))
            return;

        const QPointF scenePosition = mapToScene(viewportPosition.toPoint());
        scale(appliedFactor, appliedFactor);
        currentScale_ = nextScale;
        const QPointF anchoredPosition = mapFromScene(scenePosition);
        const QPointF offset = anchoredPosition - viewportPosition;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + qRound(offset.x()));
        verticalScrollBar()->setValue(verticalScrollBar()->value() + qRound(offset.y()));
    }

    QGraphicsScene* scene_{nullptr};
    QGraphicsItem* diagram_{nullptr};
    double currentScale_{1.0};
};

} // namespace

MermaidDiagramDialog::MermaidDiagramDialog(const QString& svgPath, const ThemePalette& palette,
                                           QWidget* parent)
    : QDialog(parent) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Mermaid Diagram"));
    resize(920, 700);
    setMinimumSize(520, 380);

    auto* hint = new QLabel(QStringLiteral("Pinch or scroll to zoom • Drag to pan"), this);
    hint->setProperty("kvRole", QStringLiteral("previewHint"));
    auto* zoomOutButton = new QPushButton(QString::fromUtf8("−"), this);
    zoomOutButton->setObjectName(QStringLiteral("mermaidZoomOut"));
    zoomOutButton->setToolTip(QStringLiteral("Zoom out"));
    auto* zoomInButton = new QPushButton(QStringLiteral("+"), this);
    zoomInButton->setObjectName(QStringLiteral("mermaidZoomIn"));
    zoomInButton->setToolTip(QStringLiteral("Zoom in"));
    auto* fitButton = new QPushButton(QStringLiteral("Fit"), this);
    auto* actualSizeButton = new QPushButton(QStringLiteral("100%"), this);
    auto* closeButton = new QPushButton(QStringLiteral("Close"), this);
    auto* view = new MermaidDiagramView(svgPath, palette, this);

    auto* toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(6);
    toolbar->addWidget(hint);
    toolbar->addStretch(1);
    toolbar->addWidget(zoomOutButton);
    toolbar->addWidget(zoomInButton);
    toolbar->addWidget(fitButton);
    toolbar->addWidget(actualSizeButton);
    toolbar->addWidget(closeButton);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);
    layout->addLayout(toolbar);
    layout->addWidget(view, 1);

    connect(zoomOutButton, &QPushButton::clicked, view, &MermaidDiagramView::zoomOut);
    connect(zoomInButton, &QPushButton::clicked, view, &MermaidDiagramView::zoomIn);
    connect(fitButton, &QPushButton::clicked, view, &MermaidDiagramView::fitDiagram);
    connect(actualSizeButton, &QPushButton::clicked, view, &MermaidDiagramView::actualSize);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    QTimer::singleShot(0, view, [view] { view->fitDiagram(); });
}

} // namespace ketplus
