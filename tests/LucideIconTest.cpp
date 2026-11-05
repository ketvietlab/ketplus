#include "ui/LucideIcon.h"
#include "agent/LucideImageProvider.h"

#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QtTest>

using namespace ketplus;

namespace {
// Rendered thickness of the horizontal bar of a plus, in device pixels.
qreal horizontalStroke(const QImage& image) {
    const int column = image.width() * 3 / 4;
    qreal coverage = 0;
    for (int y = 0; y < image.height(); ++y)
        coverage += qAlpha(image.pixel(column, y)) / 255.0;
    return coverage;
}
} // namespace

class LucideIconTest : public QObject {
    Q_OBJECT

  private slots:
    void smallIconsKeepAMinimumStroke() {
        QCOMPARE(lucide_icon::strokeWidth(24), 2.0);
        QCOMPARE(lucide_icon::strokeWidth(32), 2.0);
        QCOMPARE(lucide_icon::strokeWidth(16), 2.25);
        QCOMPARE(lucide_icon::strokeWidth(12), 3.0);
        QCOMPARE(lucide_icon::strokeWidth(0), 2.0);
        QVERIFY(lucide_icon::svg("<path d=\"M5 12h14\"/>", Qt::white, 12)
                    .contains("stroke-width=\"3\""));
    }

    void rewritesOnlyTheRootStroke() {
        const QByteArray svg = "<svg stroke-width=\"2\"><path stroke-width=\"1\"/></svg>";
        QCOMPARE(lucide_icon::withStrokeWidth(svg, 16),
                 QByteArray("<svg stroke-width=\"2.25\"><path stroke-width=\"1\"/></svg>"));
        QCOMPARE(lucide_icon::withStrokeWidth("<svg/>", 16), QByteArray("<svg/>"));
    }

    // TintedIcon colorizes by luminance, so an icon must be drawn white to take the theme color.
    // `currentColor` resolves to black in Qt SVG and left icons black in every theme.
    void agentIconsAreWhiteAndStroked() {
        int icons = 0;
        for (QDirIterator it(QStringLiteral(":/agent/icons"), {QStringLiteral("*.svg")});
             it.hasNext();) {
            QFile file(it.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray svg = file.readAll();
            QVERIFY2(!svg.contains("currentColor"), qPrintable(file.fileName()));
            QVERIFY2(svg.contains("stroke=\"#fff\""), qPrintable(file.fileName()));
            QVERIFY2(svg.contains("stroke-width=\""), qPrintable(file.fileName()));
            ++icons;
        }
        QVERIFY(icons > 20);
    }

    void providerFitsTheStrokeToTheShownSize() {
        LucideImageProvider provider;
        QSize size;
        // A 12 px icon at 2x: Lucide's own stroke would be 2 device pixels.
        const QImage small = provider.requestImage(QStringLiteral("plus/12"), &size, {24, 24});
        QCOMPARE(size, QSize(24, 24));
        QVERIFY(qAbs(horizontalStroke(small) - 2 * lucide_icon::kMinimumStrokePixels) < 0.35);
        // Large icons keep Lucide's proportion: 2/24 of 32 px at 2x.
        const QImage large = provider.requestImage(QStringLiteral("plus/32"), &size, {64, 64});
        QVERIFY(qAbs(horizontalStroke(large) - 2 * 32 * 2.0 / 24) < 0.35);
        QVERIFY(provider.requestImage(QStringLiteral("missing/12"), &size, {24, 24}).isNull());
        QVERIFY(provider.requestImage(QStringLiteral("plus"), &size, {24, 24}).isNull());
    }
};

QTEST_MAIN(LucideIconTest)
#include "LucideIconTest.moc"
