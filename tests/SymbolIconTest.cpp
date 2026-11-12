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
        // The agent resources are linked, so the removals above are real; the approved ketPlus
        // mark (1024 px with 64/128 px derivatives) replaced the Két Symbol SVG marks and tile.
        for (const char* name : {"ketplus-mark", "ketplus-mark-64", "ketplus-mark-128"}) {
            const QImage mark(QStringLiteral(":/agent/icons/%1.png").arg(QLatin1String(name)));
            QVERIFY2(!mark.isNull(), name);
            QCOMPARE(mark.width(), mark.height());
        }
        QCOMPARE(QImage(QStringLiteral(":/agent/icons/ketplus-mark.png")).width(), 1024);
        for (const char* old :
             {"ketplus-mark-light.svg", "ketplus-mark-dark.svg", "ketplus-mark-tile.svg"})
            QVERIFY2(!QFile::exists(QStringLiteral(":/agent/icons/%1").arg(QLatin1String(old))),
                     old);
        QVERIFY(!QFile::exists(QStringLiteral(":/agent/brand/ketplus-logo-light.png")));
        QVERIFY(QFile::exists(QStringLiteral(":/agent/BrandLockup.qml")));
    }
};
QTEST_MAIN(SymbolIconTest)
#include "SymbolIconTest.moc"
