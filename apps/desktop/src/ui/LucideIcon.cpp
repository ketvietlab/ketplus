#include "ui/LucideIcon.h"

#include <QColor>
#include <QPainter>
#include <QRectF>
#include <QRegularExpression>
#include <QSize>
#include <QSvgRenderer>

#include <algorithm>

namespace ketplus::lucide_icon {
namespace {
constexpr qreal kGrid = 24.0;
constexpr qreal kLucideStroke = 2.0;

QByteArray number(const qreal value) { return QByteArray::number(value, 'g', 4); }
} // namespace

qreal strokeWidth(const qreal logicalSize) {
    if (logicalSize <= 0)
        return kLucideStroke;
    return std::max(kLucideStroke, kMinimumStrokePixels * kGrid / logicalSize);
}

QByteArray svg(const QByteArray& body, const QColor& color, const qreal logicalSize) {
    return QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" "
                             "fill=\"none\" stroke=\"") +
           color.name(QColor::HexRgb).toUtf8() + QByteArrayLiteral("\" stroke-width=\"") +
           number(strokeWidth(logicalSize)) +
           QByteArrayLiteral("\" stroke-linecap=\"round\" stroke-linejoin=\"round\">") + body +
           QByteArrayLiteral("</svg>");
}

QByteArray withStrokeWidth(QByteArray svg, const qreal logicalSize) {
    static const QRegularExpression rootStroke(QStringLiteral(R"(stroke-width="[0-9.]+")"));
    const auto match = rootStroke.match(QString::fromUtf8(svg));
    if (!match.hasMatch())
        return svg;
    const QByteArray replacement = "stroke-width=\"" + number(strokeWidth(logicalSize)) + '"';
    // The SVG files are ASCII, so character offsets equal byte offsets.
    return svg.replace(match.capturedStart(), match.capturedLength(), replacement);
}

void paint(QPainter& painter, const QByteArray& body, const QColor& color, const QRectF& rect) {
    QSvgRenderer(svg(body, color, rect.width())).render(&painter, rect);
}

QPixmap pixmap(const QByteArray& body, const QColor& color, const QSize& size,
               const qreal devicePixelRatio) {
    QPixmap result(size * devicePixelRatio);
    result.setDevicePixelRatio(devicePixelRatio);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    paint(painter, body, color, QRectF(QPointF(), QSizeF(size)));
    return result;
}
} // namespace ketplus::lucide_icon
