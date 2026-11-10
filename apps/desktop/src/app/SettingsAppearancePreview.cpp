#include "app/SettingsAppearancePreview.h"

#include "KetAppearance.h"
#include "KetBadge.h"
#include "KetIcons.h"
#include "KetRichRecipes.h"
#include "KetSyntax.h"
#include "KetTokens.h"
#include <QIcon>
#include <QPainter>
#include <QPainterPath>

namespace ketplus {
namespace {

const QString previewCode = QStringLiteral("// Focus on what you are creating.\n"
                                           "property string workspace: \"develop\"\n"
                                           "color: theme.accentSubtle");
constexpr int messageHeight = 44;
constexpr int messageInset = 16;
constexpr int markSize = 18;
constexpr int frameRadius = 7;

ket::Theme theme() { return ket::Appearance::instance().theme(); }

QColor color(QStringView name) { return ket::Tokens::color(name, theme()); }

} // namespace

WorkspacePreview::WorkspacePreview(QWidget* parent)
    : QWidget(parent), badge_(new ket::Badge(tr("Completed"), ket::BadgeTone::Success, this)) {
    setObjectName(QStringLiteral("settingsWorkspacePreview"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName(tr("Workspace preview"));
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
            qOverload<>(&QWidget::update));
}

void WorkspacePreview::setCodeFamily(const QString& family) {
    if (codeFamily_ == family)
        return;
    codeFamily_ = family;
    updateGeometry();
    update();
}

QFont WorkspacePreview::codeFont() const {
    QFont code = font();
    if (!codeFamily_.isEmpty())
        code.setFamily(codeFamily_);
    code.setPixelSize(qRound(ket::codeBlockRecipe(theme()).fontPixels));
    return code;
}

int WorkspacePreview::codeLineHeight() const {
    const auto recipe = ket::codeBlockRecipe(theme());
    return qFloor(recipe.fontPixels * recipe.lineHeight);
}

int WorkspacePreview::codeBlockHeight() const {
    const auto recipe = ket::codeBlockRecipe(theme());
    const int lines = int(previewCode.count(u'\n')) + 1;
    return qRound(2 * recipe.margin + 2 * recipe.padding) + lines * codeLineHeight();
}

QSize WorkspacePreview::sizeHint() const {
    const int tabs = qRound(ket::tabBarRecipe(theme()).height);
    return {480, tabs + codeBlockHeight() + messageHeight};
}

QSize WorkspacePreview::minimumSizeHint() const { return {240, sizeHint().height()}; }

void WorkspacePreview::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const QSize badge = badge_->sizeHint();
    badge_->resize(badge);
    const int top = height() - messageHeight;
    badge_->move(width() - messageInset - badge.width(),
                 top + (messageHeight - badge.height()) / 2);
}

void WorkspacePreview::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const ket::Theme current = theme();
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath clip;
    clip.addRoundedRect(frame, frameRadius, frameRadius);
    painter.fillPath(clip, color(u"panel-bg"));
    painter.save();
    painter.setClipPath(clip);

    // Editor tabs: the open file selected with its icon, a second file beside it.
    const auto bar = ket::tabBarRecipe(current);
    const int barHeight = qRound(bar.height);
    painter.fillRect(QRect(0, 0, width(), barHeight), bar.background);
    int x = 0;
    for (const bool selected : {true, false}) {
        const auto tab = ket::tabRecipe(selected, {}, current);
        QFont tabFont = font();
        tabFont.setPixelSize(qRound(tab.fontPixels));
        const QString text =
            selected ? QStringLiteral("NavigationRail.qml") : QStringLiteral("README.md");
        const int icon = selected ? qRound(tab.iconSize + tab.spacing) : 0;
        const int tabWidth =
            qRound(2 * tab.paddingInline) + icon + QFontMetrics(tabFont).horizontalAdvance(text);
        const QRect area(x, 0, tabWidth, barHeight);
        painter.fillRect(area, tab.background);
        if (selected) {
            painter.drawPixmap(
                QPointF(x + tab.paddingInline, (barHeight - tab.iconSize) / 2),
                ket::Icons::pixmap(u"file-code", tab.iconSize, devicePixelRatioF(), tab.text));
            painter.fillRect(
                QRectF(x, barHeight - tab.indicatorHeight, tabWidth, tab.indicatorHeight),
                tab.indicator);
        }
        painter.setFont(tabFont);
        painter.setPen(tab.text);
        painter.drawText(area.adjusted(qRound(tab.paddingInline) + icon, 0, 0, 0),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
        x += tabWidth;
        painter.fillRect(QRect(x - 1, 0, 1, barHeight), bar.border);
    }
    painter.fillRect(QRect(x, barHeight - 1, width() - x, 1), bar.border);

    // The code block, highlighted as a QML fence and set in the editor font.
    const auto block = ket::codeBlockRecipe(current);
    const QRectF surface(0.5, barHeight + block.margin + 0.5, width() - 1.0,
                         codeBlockHeight() - 2 * block.margin - 1.0);
    painter.setPen(block.border);
    painter.setBrush(block.background);
    painter.drawRoundedRect(surface, block.radius, block.radius);
    const qreal copy = block.fontPixels;
    painter.drawPixmap(
        QPointF(surface.right() - block.padding / 2 - copy - 6,
                surface.top() + block.padding / 2 + 6),
        ket::Icons::pixmap(u"copy", copy, devicePixelRatioF(), color(u"text-secondary")));
    const QFont code = codeFont();
    const QFontMetrics metrics(code);
    painter.setFont(code);
    const auto syntax = ket::syntaxRecipe(current);
    const int lineHeight = codeLineHeight();
    int state = 0;
    qreal baseline =
        surface.top() + block.padding + (lineHeight + metrics.ascent() - metrics.descent()) / 2.0;
    for (const QString& line : previewCode.split(u'\n')) {
        qreal left = surface.left() + block.padding;
        qsizetype from = 0;
        const auto draw = [&](qsizetype start, qsizetype length, const QColor& ink) {
            const QString part = line.mid(start, length);
            painter.setPen(ink);
            painter.drawText(QPointF(left, baseline), part);
            left += metrics.horizontalAdvance(part);
        };
        for (const auto& span : ket::highlightLine(line, u"qml", state)) {
            draw(from, span.start - from, block.text);
            draw(span.start, span.length, ket::syntaxColor(syntax, span.role));
            from = span.start + span.length;
        }
        draw(from, line.size() - from, block.text);
        baseline += lineHeight;
    }

    // A finished agent message under the code, with the badge placed in resizeEvent().
    const int top = height() - messageHeight;
    const QString mark = current == ket::Theme::Dark
                             ? QStringLiteral(":/agent/icons/ketplus-mark-dark.svg")
                             : QStringLiteral(":/agent/icons/ketplus-mark-light.svg");
    QIcon(mark).paint(
        &painter, QRect(messageInset, top + (messageHeight - markSize) / 2, markSize, markSize));
    QFont message = font();
    message.setPixelSize(13);
    painter.setFont(message);
    painter.setPen(color(u"text-main"));
    painter.drawText(QRect(messageInset + markSize + 8, top, width(), messageHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, tr("Ready for your next idea."));
    painter.restore();

    painter.setPen(color(u"panel-border"));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(frame, frameRadius, frameRadius);
}

} // namespace ketplus
