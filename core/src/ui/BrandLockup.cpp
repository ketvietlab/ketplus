#include "ui/BrandLockup.h"

#include "KetAppearance.h"
#include "KetTokens.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>

namespace ketplus {

namespace {

constexpr qreal markScale = 1.45;    // mark side / wordmark pixel size
constexpr qreal gapScale = 0.12;     // gap / mark side
constexpr qreal badgeGapScale = 0.3; // wordmark → badge gap / wordmark pixel size
constexpr qreal badgeTextScale = 0.6;
constexpr qreal letterSpacingScale = -0.015;

QFont ketSansBold(qreal pixelSize) {
    QFont font(QStringLiteral("KET Sans"));
    font.setWeight(QFont::Bold);
    font.setPixelSize(qMax(1, qRound(pixelSize)));
    return font;
}

} // namespace

BrandLockup::BrandLockup(const Edition edition, QWidget* parent)
    : QWidget(parent), edition_(edition), mark_(QStringLiteral(":/brand/ketplus-mark-256.png")) {
    setObjectName(QStringLiteral("brandLockup"));
    setAttribute(Qt::WA_TranslucentBackground);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAccessibleName(productName());
    auto& appearance = ket::Appearance::instance();
    connect(&appearance, &ket::Appearance::themeChanged, this, [this] { update(); });
    connect(&appearance, &ket::Appearance::zoomChanged, this,
            &BrandLockup::geometryChangedByAppearance);
}

QString BrandLockup::productName() const {
    return edition_ == Edition::Pro ? QStringLiteral("ketPlus Pro") : QStringLiteral("ketPlus");
}

void BrandLockup::setWordSize(const qreal logicalPixels) {
    if (qFuzzyCompare(wordSize_, logicalPixels))
        return;
    wordSize_ = logicalPixels;
    geometryChangedByAppearance();
}

void BrandLockup::geometryChangedByAppearance() {
    updateGeometry();
    resize(sizeHint());
    update();
}

qreal BrandLockup::scaledWordSize() const { return ket::Appearance::instance().scaled(wordSize_); }

qreal BrandLockup::markSize() const { return scaledWordSize() * markScale; }

QFont BrandLockup::wordFont() const {
    QFont font = ketSansBold(scaledWordSize());
    font.setLetterSpacing(QFont::AbsoluteSpacing, scaledWordSize() * letterSpacingScale);
    return font;
}

QFont BrandLockup::badgeFont() const { return ketSansBold(scaledWordSize() * badgeTextScale); }

QColor BrandLockup::textColor() const {
    return ket::Tokens::color(u"text-main", ket::Appearance::instance().theme());
}

QColor BrandLockup::brandColor() const {
    return ket::Tokens::color(u"accent-text", ket::Appearance::instance().theme());
}

QSizeF BrandLockup::badgeSize() const {
    if (edition_ != Edition::Pro)
        return {};
    const QFont font = badgeFont();
    const QFontMetricsF metrics(font);
    const qreal pixels = font.pixelSize();
    return {metrics.horizontalAdvance(QStringLiteral("Pro")) + pixels * 0.7,
            metrics.height() + pixels * 0.24};
}

QSizeF BrandLockup::contentSize() const {
    const QFontMetricsF word(wordFont());
    const qreal width =
        markSize() * (1 + gapScale) + word.horizontalAdvance(QStringLiteral("ketPlus")) +
        (edition_ == Edition::Pro ? scaledWordSize() * badgeGapScale + badgeSize().width() : 0);
    const qreal height = qMax(qMax(markSize(), word.height()), badgeSize().height());
    return {width, height};
}

QSize BrandLockup::sizeHint() const {
    const QSizeF size = contentSize();
    return {qCeil(size.width()), qCeil(size.height())};
}

QRectF BrandLockup::markRect() const {
    return {0, (height() - markSize()) / 2, markSize(), markSize()};
}

QRectF BrandLockup::wordRect() const {
    const QFontMetricsF word(wordFont());
    const qreal wordHeight = word.height();
    return {markSize() * (1 + gapScale), (height() - wordHeight) / 2,
            word.horizontalAdvance(QStringLiteral("ketPlus")), wordHeight};
}

QRectF BrandLockup::badgeRect() const {
    if (edition_ != Edition::Pro)
        return {};
    const QRectF word = wordRect();
    const QFontMetricsF metrics(wordFont());
    const qreal baseline = word.top() + metrics.ascent();
    const qreal centre = baseline - metrics.xHeight() / 2;
    const QSizeF size = badgeSize();
    return {word.right() + scaledWordSize() * badgeGapScale, centre - size.height() / 2,
            size.width(), size.height()};
}

void BrandLockup::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform |
                           QPainter::TextAntialiasing);
    if (!mark_.isNull())
        painter.drawPixmap(markRect(), mark_, QRectF(mark_.rect()));

    const QFont font = wordFont();
    const QFontMetricsF metrics(font);
    const QRectF word = wordRect();
    const qreal baseline = word.top() + metrics.ascent();
    painter.setFont(font);
    painter.setPen(textColor());
    const QString ket = QStringLiteral("ket");
    painter.drawText(QPointF(word.left(), baseline), ket);
    painter.setPen(brandColor());
    painter.drawText(QPointF(word.left() + metrics.horizontalAdvance(ket), baseline),
                     QStringLiteral("Plus"));

    if (edition_ != Edition::Pro)
        return;
    const QRectF badge = badgeRect();
    const qreal stroke = 1.5 * ket::Appearance::instance().zoom();
    QPen pen(brandColor(), stroke);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const QRectF frame = badge.adjusted(stroke / 2, stroke / 2, -stroke / 2, -stroke / 2);
    painter.drawRoundedRect(frame, badge.height() * 0.25, badge.height() * 0.25);
    painter.setFont(badgeFont());
    painter.drawText(badge, Qt::AlignCenter, QStringLiteral("Pro"));
}

} // namespace ketplus
