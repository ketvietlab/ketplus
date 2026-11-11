#include "ui/SymbolIcon.h"
#include "KetAppearance.h"
#include "KetIcons.h"
#include "KetRecipes.h"
#include <QPainter>
namespace ketplus::symbol_icon {
QPixmap pixmap(const QByteArray& name, const QColor& color, const QSize& size,
               qreal devicePixelRatio) {
    const QString key = QString::fromUtf8(name);
    const QString code = ket::Icons::contains(key) ? key : ket::Icons::codeForLucide(key);
    if (code.isEmpty()) {
        qWarning("Unknown Két Symbol name: %s", name.constData());
        return {};
    }
    return ket::Icons::pixmap(code, qMin(size.width(), size.height()), devicePixelRatio, color);
}
void paint(QPainter& painter, const QByteArray& name, const QColor& color, const QRectF& rect) {
    const qreal dpr = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    const auto image = pixmap(name, color, rect.size().toSize(), dpr);
    const QSizeF logical = image.deviceIndependentSize();
    painter.drawPixmap(rect.center() - QPointF(logical.width() / 2, logical.height() / 2), image);
}
QColor primaryButtonInk() {
    return ket::buttonRecipe(ket::ButtonVariant::Primary, ket::ControlSize::Medium, {},
                             ket::Appearance::instance().theme())
        .text;
}
} // namespace ketplus::symbol_icon
