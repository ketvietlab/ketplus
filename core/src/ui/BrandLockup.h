#pragma once

#include <QPixmap>
#include <QWidget>

namespace ketplus {

// The approved ketPlus / ketPlus Pro lockup for widget surfaces, the widget twin of
// mockups/qt/qml/parts/BrandLockup.qml: the mark at 1.45× the wordmark pixel size, a gap of
// 0.12× the mark, `ketPlus` in KÉT Sans Bold 700 with "Plus" in the accent-text token, and for
// Pro a framed "Pro" badge (1.5 px stroke, transparent fill) centred on the x-height.
// Colours come from ket::Tokens and follow the theme; sizes follow the interface zoom.
// The mark is read from :/brand/ketplus-mark-256.png, which each application embeds.
class BrandLockup : public QWidget {
    Q_OBJECT

  public:
    enum class Edition { Lite, Pro };

    explicit BrandLockup(Edition edition, QWidget* parent = nullptr);

    [[nodiscard]] Edition edition() const { return edition_; }
    // The wordmark's pixel size at 100% zoom; the mock's sidebar uses 17.
    void setWordSize(qreal logicalPixels);
    [[nodiscard]] qreal wordSize() const { return wordSize_; }
    [[nodiscard]] QString productName() const;

    // Geometry at the current zoom, exposed for tests.
    [[nodiscard]] qreal scaledWordSize() const;
    [[nodiscard]] qreal markSize() const;
    [[nodiscard]] QRectF markRect() const;
    [[nodiscard]] QRectF wordRect() const;
    [[nodiscard]] QRectF badgeRect() const; // null for Lite
    [[nodiscard]] QFont wordFont() const;
    [[nodiscard]] QFont badgeFont() const;
    [[nodiscard]] QColor textColor() const;
    [[nodiscard]] QColor brandColor() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override { return sizeHint(); }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    [[nodiscard]] QSizeF contentSize() const;
    [[nodiscard]] QSizeF badgeSize() const;
    void geometryChangedByAppearance();

    Edition edition_;
    qreal wordSize_ = 17;
    QPixmap mark_;
};

} // namespace ketplus
