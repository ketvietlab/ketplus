#include "ui/SymbolIcon.h"
#include "KetIcons.h"
#include "agent/SymbolImageProvider.h"
#include <QtTest>
using namespace ketplus;
class SymbolIconTest final : public QObject {
    Q_OBJECT
  private slots:
    void drawsTheKitMasterAtEachScale() {
        for (int box : {12, 14, 16, 18, 20, 24, 32}) {
            for (qreal dpr : {1.0, 2.0}) {
                const auto image = symbol_icon::pixmap("plus", Qt::blue, {box, box}, dpr).toImage();
                QCOMPARE(image.size(), QSize(qRound(box * dpr), qRound(box * dpr)));
                int ink = 0;
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x) {
                        const QColor pixel = image.pixelColor(x, y);
                        if (pixel.alpha() > 128) {
                            ++ink;
                            QCOMPARE(pixel.blue(), 255);
                            QCOMPARE(pixel.red(), 0);
                        }
                    }
                QVERIFY(ink > 0);
                QVERIFY(ink < image.width() * image.height());
            }
        }
    }
    void providerResolvesNamesWithoutLegacySvgResources() {
        SymbolImageProvider provider;
        QSize size;
        for (const auto* name : {"search", "plus", "git-branch", "messages-square", "shield",
                                 "square-terminal", "layers", "monitor"}) {
            const auto image =
                provider.requestImage(QString::fromLatin1(name) + "/16", &size, {32, 32});
            QVERIFY2(!image.isNull(), name);
            QCOMPARE(size, QSize(32, 32));
        }
        QVERIFY(provider.requestImage("missing/16", &size, {}).isNull());
        QVERIFY(provider.requestImage("plus/0", &size, {}).isNull());
        QVERIFY(provider.requestImage("plus/99999", &size, {}).isNull());
    }
};
QTEST_MAIN(SymbolIconTest)
#include "SymbolIconTest.moc"
