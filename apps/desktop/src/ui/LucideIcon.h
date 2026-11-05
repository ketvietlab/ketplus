#pragma once

#include <QByteArray>
#include <QPixmap>

class QColor;
class QPainter;
class QRectF;
class QSize;

namespace ketplus::lucide_icon {
// Lucide draws a 2-unit stroke on a 24-unit grid, so the line thins with the icon: a 12 px chevron
// gets a 1 px hairline. Like Lucide's absoluteStrokeWidth, strokes never render thinner than this
// many logical pixels; icons large enough keep Lucide's own proportion.
inline constexpr qreal kMinimumStrokePixels = 1.5;

// Stroke width in viewBox units for an icon drawn `logicalSize` pixels wide.
qreal strokeWidth(qreal logicalSize);
// A complete SVG document for a Lucide path body drawn `logicalSize` pixels wide.
QByteArray svg(const QByteArray& body, const QColor& color, qreal logicalSize);
// Rewrites the root stroke width of a complete Lucide SVG file for `logicalSize`.
QByteArray withStrokeWidth(QByteArray svg, qreal logicalSize);
void paint(QPainter& painter, const QByteArray& body, const QColor& color, const QRectF& rect);
QPixmap pixmap(const QByteArray& body, const QColor& color, const QSize& size,
               qreal devicePixelRatio = 2.0);
} // namespace ketplus::lucide_icon
