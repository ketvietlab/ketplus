#include "ui/SymbolIcon.h"
#include "KetIcons.h"
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

    void mapsLegacyNamesWithoutSvgResources() {
        for (const char* name :
             {"plus", "x", "chevron-down", "copy", "git-branch", "network", "bot", "search",
              "square-terminal", "file-code", "shield", "lock"}) {
            const auto code = ket::Icons::codeForLucide(QString::fromLatin1(name));
            QVERIFY2(!code.isEmpty(), name);
            QVERIFY2(!ket::Icons::icon(code).isNull(), name);
        }
        QVERIFY(!QFile::exists(QStringLiteral(":/agent/icons/plus.svg")));
        QVERIFY(!QFile::exists(QStringLiteral(":/atlas/icons/plus.svg")));
    }
};
QTEST_MAIN(SymbolIconTest)
#include "SymbolIconTest.moc"
