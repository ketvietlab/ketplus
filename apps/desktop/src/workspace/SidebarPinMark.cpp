#include "workspace/SidebarPinMark.h"

#include "KetAppearance.h"
#include "KetSidebarTree.h"
#include "KetTokens.h"

#include <QEvent>
#include <QFontMetrics>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>

namespace ketplus {
namespace {

// Logical pixels at 100% zoom.
constexpr qreal kMarkSize = 11;

// The header's name font: its recipe's size, weight and spacing, in capitals.
QFont nameFontOf(const ket::SidebarRepositoryHeader& header) {
    const ket::SidebarRepositoryRecipe r = header.recipe();
    const ket::Appearance& appearance = ket::Appearance::instance();
    QFont font = header.font();
    font.setPixelSize(qRound(appearance.scaled(r.namePixels)));
    font.setWeight(QFont::Weight(r.nameWeight));
    font.setCapitalization(QFont::AllUppercase);
    font.setLetterSpacing(QFont::AbsoluteSpacing, r.letterSpacing * appearance.zoom());
    return font;
}

} // namespace

SidebarPinMark::SidebarPinMark(ket::SidebarRepositoryHeader* header)
    : QWidget(header), header_(header), fullName_(header->text()) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this, [this] { place(); });
    // The header names itself again when it folds or unfolds; it keeps the full name.
    connect(header, &QAbstractButton::toggled, this,
            [this] { header_->setAccessibleName(fullName_); });
}

void SidebarPinMark::place() {
    if (placing_)
        return;
    placing_ = true;
    const qreal zoom = ket::Appearance::instance().zoom();
    const int side = qRound(kMarkSize * zoom);
    const int gap = qRound(3 * zoom);
    const QRect name = header_->nameRect();
    const QFontMetrics metrics(nameFontOf(*header_));
    // Room for the name once the pin and its gap are kept free at the end of the name area.
    const int room = qMax(0, name.width() - side - gap);
    const QString shown = metrics.horizontalAdvance(fullName_) <= room
                              ? fullName_
                              : metrics.elidedText(fullName_, Qt::ElideMiddle, room);
    if (header_->text() != shown)
        header_->setText(shown);
    header_->setAccessibleName(fullName_);
    const int textRight = name.left() + qMin(metrics.horizontalAdvance(shown), room);
    setFixedSize(side, side);
    move(textRight + gap, name.center().y() - side / 2);
    update();
    placing_ = false;
}

void SidebarPinMark::placeLater() {
    // The header lays out its count and action in a queued call after focus and hover changes.
    QMetaObject::invokeMethod(this, &SidebarPinMark::place, Qt::QueuedConnection);
}

bool SidebarPinMark::eventFilter(QObject* watched, QEvent* event) {
    if (watched == header_) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::LayoutRequest:
        case QEvent::Enter:
        case QEvent::Leave:
        case QEvent::FocusIn:
        case QEvent::FocusOut:
            placeLater();
            break;
        default:
            break;
        }
    }
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
