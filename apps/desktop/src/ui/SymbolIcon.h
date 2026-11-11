#pragma once
#include <QByteArray>
#include <QPixmap>
class QColor;
class QPainter;
class QRectF;
class QSize;
namespace ketplus::symbol_icon {
// Semantic names use the kit mapping; master size and weight belong to Két Symbols.
void paint(QPainter& painter, const QByteArray& name, const QColor& color, const QRectF& rect);
QPixmap pixmap(const QByteArray& name, const QColor& color, const QSize& size,
               qreal devicePixelRatio = 2.0);
// The kit's primary button text, for an icon drawn on that button in the current theme.
QColor primaryButtonInk();
} // namespace ketplus::symbol_icon
