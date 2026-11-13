#include "workspace/SidebarPinMark.h"

#include "KetAppearance.h"
#include "KetSidebarTree.h"
#include "KetTokens.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>

namespace ketplus {
namespace {

// Logical pixels at 100% zoom.
constexpr qreal kMarkSize = 11;

} // namespace

SidebarPinMark::SidebarPinMark(ket::SidebarRepositoryHeader* header)
    : QWidget(header), header_(header) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this, [this] { place(); });
}

void SidebarPinMark::place() {
    const int side = qRound(kMarkSize * ket::Appearance::instance().zoom());
    const QRect name = header_->nameRect();
    setFixedSize(side, side);
    // Just past the name's area, before the count: an elided name never runs under it.
    move(name.right() + qRound(3 * ket::Appearance::instance().zoom()),
         name.center().y() - side / 2);
    update();
}

bool SidebarPinMark::eventFilter(QObject* watched, QEvent* event) {
    if (watched == header_ && (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
                               event->type() == QEvent::LayoutRequest))
        place();
    return QWidget::eventFilter(watched, event);
}

void SidebarPinMark::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color = ket::Tokens::color(u"text-muted", ket::Appearance::instance().theme());
    // A push pin leaning right: a round head, a collar and a needle, in a 12-unit box.
    const qreal unit = width() / 12.0;
    painter.scale(unit, unit);
    painter.translate(6, 6);
    painter.rotate(45);
    painter.translate(-6, -6);
    QPen pen(color, 1.3);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    QPainterPath head;
    head.moveTo(4, 1);
    head.lineTo(8, 1);
    head.lineTo(7.4, 5);
    head.lineTo(9, 7);
    head.lineTo(3, 7);
    head.lineTo(4.6, 5);
    head.closeSubpath();
    painter.setBrush(color);
    painter.drawPath(head);
    painter.drawLine(QPointF(6, 7), QPointF(6, 11));
}

} // namespace ketplus
