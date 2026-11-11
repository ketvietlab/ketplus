#include "teams/TeamPageParts.h"

#include "KetAppearance.h"
#include "KetBadge.h"
#include "KetIcon.h"
#include "KetTokens.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTableWidget>
#include <QTextLayout>

#include <array>

namespace ketplus::team_page {
namespace {

QColor token(const QString& name) {
    return ket::Tokens::color(name, ket::Appearance::instance().theme());
}

QFont typeFont(QFont font, const qreal pixels, const int weight, const bool mono) {
    if (mono)
        if (const QString family = ket::Tokens::availableFamily(u"font-mono"); !family.isEmpty())
            font.setFamily(family);
    font.setPixelSize(px(pixels));
    font.setWeight(static_cast<QFont::Weight>(weight));
    return font;
}

} // namespace

int px(const qreal logical) { return qRound(ket::Appearance::instance().scaled(logical)); }

Text::Text(const QString& value, QWidget* parent, const Fit fit)
    : QLabel(value, parent), fit_(fit) {
    setTextFormat(Qt::PlainText);
    QSizePolicy policy(QSizePolicy::Preferred,
                       fit == Fit::Wrap ? QSizePolicy::Preferred : QSizePolicy::Fixed);
    policy.setHeightForWidth(fit == Fit::Wrap);
    setSizePolicy(policy);
    setType(13);
}

void Text::setTone(const QString& tone) {
    tone_ = tone;
    update();
}

void Text::setType(const qreal pixels, const int weight, const bool mono) {
    setFont(typeFont(font(), pixels, weight, mono));
    updateGeometry();
}

void Text::setLetterSpacing(const qreal pixels) {
    QFont spaced = font();
    spaced.setLetterSpacing(QFont::AbsoluteSpacing, ket::Appearance::instance().scaled(pixels));
    setFont(spaced);
    updateGeometry();
}

// Lays the text out in `width`, breaking inside a word only when the word alone is too wide.
void Text::setMaximumLines(const int lines) {
    maximumLines_ = lines;
    updateGeometry();
    measure();
    update();
}

void Text::measure() {
    bool cut = false;
    if (fit_ == Fit::Wrap)
        lay(contentsRect().width(), nullptr, &cut);
    if (cut != truncated_) {
        truncated_ = cut;
        emit truncatedChanged(cut);
    }
}

qreal Text::lay(const int width, QPainter* painter, bool* cut) const {
    QString shown = text();
    shown.replace(QLatin1Char('\n'), QChar::LineSeparator);
    QTextLayout layout(shown, font());
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    const qreal gap = px(3);
    qreal y = 0;
    layout.beginLayout();
    int lines = 0;
    int rest = -1; // where the text the allowed lines leave out starts
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(qMax(1, width));
        if (maximumLines_ > 0 && lines == maximumLines_) {
            rest = line.textStart();
            break;
        }
        line.setPosition(QPointF(0, y));
        y += line.height() + gap;
        ++lines;
    }
    layout.endLayout();
    if (cut)
        *cut = rest >= 0;
    if (painter) {
        const QPointF origin = contentsRect().topLeft();
        for (int index = 0; index < lines; ++index) {
            const QTextLine line = layout.lineAt(index);
            if (rest < 0 || index < lines - 1) {
                line.draw(painter, origin);
                continue;
            }
            // The last line allowed ends in an ellipsis for what follows it.
            const QString tail = shown.mid(line.textStart()).replace(QChar::LineSeparator, u' ');
            painter->drawText(QRectF(origin.x(), origin.y() + line.y(), width, line.height()),
                              Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                              fontMetrics().elidedText(tail, Qt::ElideRight, width));
        }
    }
    return qMax<qreal>(fontMetrics().height(), y - gap);
}

QSize Text::sizeHint() const {
    const QFontMetrics metrics(font());
    int width = 0;
    for (const QString& line : text().split(QLatin1Char('\n')))
        width = qMax(width, metrics.horizontalAdvance(line) + 1);
    if (fit_ != Fit::Wrap)
        return {width, metrics.height()};
    width = qMin(width, px(1120));
    return {width, heightForWidth(width)};
}

QSize Text::minimumSizeHint() const {
    return {fit_ == Fit::Wrap ? px(40) : px(16), fontMetrics().height()};
}

int Text::heightForWidth(const int width) const {
    return fit_ == Fit::Wrap ? qCeil(lay(width, nullptr)) : fontMetrics().height();
}

void Text::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setPen(token(tone_));
    painter.setFont(font());
    if (fit_ == Fit::Wrap) {
        lay(contentsRect().width(), &painter);
        return;
    }
    const Qt::TextElideMode mode = fit_ == Fit::ElideMiddle ? Qt::ElideMiddle : Qt::ElideRight;
    painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                     fontMetrics().elidedText(text(), mode, contentsRect().width()));
}

void Text::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    measure();
    // The whole value stays reachable when the line is cut.
    if (fit_ != Fit::Wrap && toolTip().isEmpty() == (sizeHint().width() > width()))
        setToolTip(sizeHint().width() > width() ? text() : QString{});
}

Panel::Panel(QWidget* parent, const bool accent) : QFrame(parent), accent_(accent) {
    setFrameShape(QFrame::NoFrame);
}

void Panel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(
        QPen(token(accent_ ? QStringLiteral("accent-border") : QStringLiteral("panel-border")), 1));
    painter.setBrush(token(QStringLiteral("panel-bg")));
    const qreal radius = px(7);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

Row::Row(QWidget* parent, const bool ruled) : QWidget(parent), ruled_(ruled) {}

void Row::paintEvent(QPaintEvent*) {
    if (!ruled_)
        return;
    QPainter painter(this);
    painter.fillRect(QRect(0, 0, width(), 1), token(QStringLiteral("panel-border")));
}

Flow::Flow(QWidget* parent, const int horizontalGap, const int verticalGap)
    : QLayout(parent), horizontalGap_(horizontalGap), verticalGap_(verticalGap) {
    setContentsMargins(0, 0, 0, 0);
}

Flow::~Flow() {
    while (QLayoutItem* item = takeAt(0))
        delete item;
}

void Flow::addItem(QLayoutItem* item) { items_.append(item); }

QLayoutItem* Flow::takeAt(const int index) {
    return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
}

int Flow::heightForWidth(const int width) const { return place(QRect(0, 0, width, 0), false); }

void Flow::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    place(rect, true);
}

QSize Flow::sizeHint() const {
    int width = 0;
    int height = 0;
    for (const QLayoutItem* item : items_) {
        if (item->isEmpty())
            continue;
        width += item->sizeHint().width() + (width > 0 ? horizontalGap_ : 0);
        height = qMax(height, item->sizeHint().height());
    }
    return {width, height};
}

QSize Flow::minimumSize() const {
    QSize size;
    for (const QLayoutItem* item : items_)
        if (!item->isEmpty())
            size = size.expandedTo(item->minimumSize());
    return size;
}

// Returns the height the items need in `rect`'s width; with `apply`, also places them, each
// centred on its line.
int Flow::place(const QRect& rect, const bool apply) const {
    int x = rect.x();
    int y = rect.y();
    int lineHeight = 0;
    QList<std::pair<QLayoutItem*, QRect>> line;
    const auto finish = [&] {
        if (apply)
            for (const auto& [item, box] : std::as_const(line))
                item->setGeometry(box.translated(0, (lineHeight - box.height()) / 2));
        line.clear();
    };
    for (QLayoutItem* item : items_) {
        if (item->isEmpty())
            continue;
        const int width = qMin(item->sizeHint().width(), rect.width());
        const int height =
            item->hasHeightForWidth() ? item->heightForWidth(width) : item->sizeHint().height();
        if (x > rect.x() && x + width > rect.x() + rect.width()) {
            finish();
            x = rect.x();
            y += lineHeight + verticalGap_;
            lineHeight = 0;
        }
        lineHeight = qMax(lineHeight, height);
        line.append({item, QRect(x, y, width, height)});
        x += width + horizontalGap_;
    }
    finish();
    return y + lineHeight - rect.y();
}

ChoiceButton::ChoiceButton(const QString& label, const ket::ButtonVariant variant, QWidget* parent)
    : ket::Button(label, variant, parent), label_(label), full_(ket::Button::sizeHint()) {
    setAccessibleName(label);
}

QSize ChoiceButton::minimumSizeHint() const {
    return {qMin(full_.width(), px(96)), full_.height()};
}

void ChoiceButton::resizeEvent(QResizeEvent* event) {
    ket::Button::resizeEvent(event);
    if (width() >= full_.width()) {
        if (text() != label_)
            setText(label_);
        setToolTip({});
        return;
    }
    // The longest beginning of the label that fits, measured by the button itself.
    qsizetype fits = 1;
    qsizetype tooLong = label_.size();
    while (tooLong - fits > 1) {
        const qsizetype half = (fits + tooLong) / 2;
        setText(label_.left(half).trimmed() + QChar(0x2026));
        (ket::Button::sizeHint().width() <= width() ? fits : tooLong) = half;
    }
    setText(label_.left(fits).trimmed() + QChar(0x2026));
    setToolTip(label_);
}

ChoiceRow::ChoiceRow(const QString& label, const bool recommended, QWidget* parent)
    : QAbstractButton(parent), recommended_(recommended) {
    setText(label);
    setAccessibleName(recommended ? tr("%1, recommended").arg(label) : label);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    auto* line = new QHBoxLayout(this);
    line->setContentsMargins(px(14), px(10), px(12), px(10));
    line->setSpacing(px(12));
    auto* sentence = new Text(label, this);
    sentence->setType(13, recommended ? QFont::Medium : QFont::Normal);
    line->addWidget(sentence, 1);
    if (recommended)
        line->addWidget(new ket::Badge(tr("Recommended"), ket::BadgeTone::Accent, this));
    auto* chevron = new ket::Icon(QStringLiteral("chevron-right"), 14, this);
    chevron->setColor(token(QStringLiteral("text-muted")));
    line->addWidget(chevron);
    // The row is the button; what it shows takes no click of its own.
    for (QWidget* child : findChildren<QWidget*>())
        child->setAttribute(Qt::WA_TransparentForMouseEvents);
}

void ChoiceRow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool focused = hasFocus();
    painter.setBrush(isDown()       ? token(QStringLiteral("surface-inset"))
                     : underMouse() ? token(QStringLiteral("surface-hover"))
                     : recommended_ ? token(QStringLiteral("accent-subtle"))
                                    : QColor(Qt::transparent));
    painter.setPen(QPen(token(focused        ? QStringLiteral("focus-border")
                              : recommended_ ? QStringLiteral("accent-border")
                                             : QStringLiteral("panel-border")),
                        focused ? 2 : 1));
    const qreal inset = focused ? 1 : 0.5;
    const qreal radius = px(6);
    painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset), radius, radius);
}

void ChoiceRow::enterEvent(QEnterEvent* event) {
    QAbstractButton::enterEvent(event);
    update();
}

void ChoiceRow::leaveEvent(QEvent* event) {
    QAbstractButton::leaveEvent(event);
    update();
}

TaskRows::TaskRows(QTableWidget* table) : QStyledItemDelegate(table), table_(table) {
    table->setItemDelegate(this);
    table->viewport()->setMouseTracking(true);
    table->viewport()->setCursor(Qt::PointingHandCursor);
    table->viewport()->installEventFilter(this);
    table->installEventFilter(this);
    connect(table, &QTableWidget::cellClicked, this, [this](const int row) { emit chosen(row); });
}

void TaskRows::paint(QPainter* painter, const QStyleOptionViewItem& option,
                     const QModelIndex& index) const {
    const QRect cell = option.rect;
    const int column = index.column();
    const QHeaderView* header = table_->horizontalHeader();
    const bool first = header->visualIndex(column) == 0;
    bool last = true;
    for (int next = column + 1; next < header->count(); ++next)
        last = last && header->isSectionHidden(next);
    painter->save();
    if (index.row() == hovered_ || option.state.testFlag(QStyle::State_Selected))
        painter->fillRect(cell, token(QStringLiteral("surface-hover")));
    if (index.row() > 0)
        painter->fillRect(QRect(cell.left(), cell.top(), cell.width(), 1),
                          token(QStringLiteral("panel-border")));
    if (table_->hasFocus() && table_->currentRow() == index.row()) {
        // The focus ring of the row, drawn a cell at a time.
        const QColor ring = token(QStringLiteral("focus-border"));
        painter->fillRect(QRect(cell.left(), cell.top(), cell.width(), 2), ring);
        painter->fillRect(QRect(cell.left(), cell.bottom() - 1, cell.width(), 2), ring);
        if (first)
            painter->fillRect(QRect(cell.left(), cell.top(), 2, cell.height()), ring);
        if (last)
            painter->fillRect(QRect(cell.right() - 1, cell.top(), 2, cell.height()), ring);
    }
    if (column != kStateColumn) {
        const bool code = column == 0 || column == 6;
        const bool title = column == 1;
        painter->setFont(typeFont(option.font,
                                  title  ? 14
                                  : code ? 12
                                         : 13,
                                  code ? QFont::Medium : QFont::Normal, code));
        painter->setPen(
            token(title ? QStringLiteral("text-main") : QStringLiteral("text-secondary")));
        const QRect inner = cell.adjusted(first ? px(18) : px(8), 0, last ? -px(18) : -px(8), 0);
        painter->drawText(inner, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                          painter->fontMetrics().elidedText(index.data().toString(), Qt::ElideRight,
                                                            inner.width()));
    }
    painter->restore();
}

void TaskRows::fit() {
    // Lane, owner, pull request, criteria: what a narrow page gives up, in this order.
    static constexpr std::array<int, 4> optional{4, 3, 6, 2};
    QHeaderView* header = table_->horizontalHeader();
    for (const int column : optional)
        header->setSectionHidden(column, false);
    int needed = px(140); // the title
    for (int column = 0; column < header->count(); ++column)
        if (column != 1)
            needed += header->sectionSize(column);
    for (const int column : optional) {
        if (needed <= table_->viewport()->width())
            break;
        needed -= header->sectionSize(column);
        header->setSectionHidden(column, true);
    }
}

bool TaskRows::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_->viewport()) {
        int row = hovered_;
        if (event->type() == QEvent::MouseMove)
            row = table_->rowAt(static_cast<QMouseEvent*>(event)->position().toPoint().y());
        else if (event->type() == QEvent::Leave)
            row = -1;
        else if (event->type() == QEvent::Resize)
            fit();
        if (row != hovered_) {
            hovered_ = row;
            table_->viewport()->update();
        }
    } else if (event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if ((key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) &&
            table_->currentRow() >= 0) {
            emit chosen(table_->currentRow());
            return true;
        }
    } else if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
        table_->viewport()->update();
    }
    return false;
}

} // namespace ketplus::team_page
