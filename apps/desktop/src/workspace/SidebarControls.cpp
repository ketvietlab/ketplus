#include "workspace/SidebarControls.h"

#include "KetAppearance.h"
#include "KetIcons.h"
#include "KetTokens.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>

namespace ketplus {
namespace {

qreal zoomed(qreal logical) {
    return ket::Appearance::scale(logical, ket::Appearance::instance().zoom());
}

QColor token(QStringView name) {
    return ket::Tokens::color(name, ket::Appearance::instance().theme());
}

QFont sized(QFont font, qreal pixels, QFont::Weight weight) {
    font.setPixelSize(qRound(zoomed(pixels)));
    font.setWeight(weight);
    return font;
}

} // namespace

SidebarSearchButton::SidebarSearchButton(const QString& text, const QString& shortcut,
                                         QWidget* parent)
    : QAbstractButton(parent), shortcut_(shortcut) {
    setText(text);
    setAccessibleName(text);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const auto refresh = [this] {
        updateGeometry();
        update();
    };
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this, refresh);
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this, refresh);
}

QSize SidebarSearchButton::sizeHint() const { return {qRound(zoomed(120)), qRound(zoomed(28))}; }

bool SidebarSearchButton::event(QEvent* event) {
    if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave)
        update();
    return QAbstractButton::event(event);
}

void SidebarSearchButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool focused = hasFocus() && focusPolicy() != Qt::NoFocus;
    const bool hovered = underMouse();
    // The mock's field: input fill, its hover fill, and the focus border at 2 px.
    const qreal border = focused ? 2 : 1;
    const QRectF frame = QRectF(rect()).adjusted(border / 2, border / 2, -border / 2, -border / 2);
    painter.setPen(QPen(token(focused ? u"focus-border" : u"input-border"), border));
    painter.setBrush(token(hovered ? u"input-bg-hover" : u"input-bg"));
    painter.drawRoundedRect(frame, zoomed(6), zoomed(6));

    const QColor muted = token(u"text-muted");
    const qreal box = zoomed(12);
    const QPixmap glyph =
        ket::Icons::pixmap(ket::Icons::codeForLucide(u"search"), box, devicePixelRatioF(), muted);
    painter.drawPixmap(QPointF(zoomed(9), (height() - box) / 2), glyph);

    painter.setPen(muted);
    const QFont shortcutFont = sized(font(), 11, QFont::Medium);
    const qreal shortcutWidth = QFontMetricsF(shortcutFont).horizontalAdvance(shortcut_);
    const QRectF shortcutRect(width() - zoomed(9) - shortcutWidth, 0, shortcutWidth, height());
    painter.setFont(shortcutFont);
    painter.drawText(shortcutRect, Qt::AlignVCenter | Qt::AlignRight, shortcut_);

    const QFont labelFont = sized(font(), 12, QFont::Medium);
    painter.setFont(labelFont);
    const QRectF labelRect(zoomed(29), 0, shortcutRect.left() - zoomed(29) - zoomed(6), height());
    painter.drawText(
        labelRect, Qt::AlignVCenter | Qt::AlignLeft,
        QFontMetricsF(labelFont).elidedText(text(), Qt::ElideRight, labelRect.width()));
}

} // namespace ketplus
