#include "ui/BrandLockup.h"
#include "KetAppearance.h"
#include "KetTokens.h"
#include "ui/AboutDialog.h"
#include "ui/KetFonts.h"

#include <QFontInfo>
#include <QLabel>
#include <QTest>

using ketplus::BrandLockup;

// The approved geometry from mockups/qt/qml/parts/BrandLockup.qml.
class BrandLockupTest : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        Q_INIT_RESOURCE(ketplus_brand_lockup_fonts);
        Q_INIT_RESOURCE(ketplus_brand_lockup_fonts_mono);
        Q_INIT_RESOURCE(ketplus_brand_lockup_mark);
        QVERIFY(ketplus::registerKetFonts().contains(QStringLiteral("KET Sans")));
    }

    void geometryFollowsTheApprovedRatios_data() {
        QTest::addColumn<qreal>("zoom");
        QTest::newRow("100%") << 1.0;
        QTest::newRow("150%") << 1.5;
        QTest::newRow("300%") << 3.0;
    }
    void geometryFollowsTheApprovedRatios() {
        QFETCH(qreal, zoom);
        auto& appearance = ket::Appearance::instance();
        appearance.setZoom(zoom);
        BrandLockup lockup(BrandLockup::Edition::Pro);
        lockup.setWordSize(17);
        lockup.resize(lockup.sizeHint());
        const qreal word = ket::Appearance::scale(17, zoom);
        QCOMPARE(lockup.scaledWordSize(), word);
        QCOMPARE(lockup.markRect().width(), word * 1.45);
        QCOMPARE(lockup.markRect().height(), word * 1.45);
        QVERIFY(qAbs(lockup.wordRect().left() - lockup.markRect().right() -
                     lockup.markRect().width() * 0.12) < 0.001);
        const QFontInfo info(lockup.wordFont());
        QCOMPARE(info.family(), QStringLiteral("KET Sans"));
        QCOMPARE(info.weight(), int(QFont::Bold));
        // The badge sits after the wordmark, inside the widget, centred on the x-height.
        const QRectF badge = lockup.badgeRect();
        QVERIFY(!badge.isNull());
        QVERIFY(badge.left() > lockup.wordRect().right());
        QVERIFY(badge.right() <= lockup.width() + 0.5);
        QVERIFY(badge.top() >= -0.5 && badge.bottom() <= lockup.height() + 0.5);
        QCOMPARE(lockup.badgeFont().weight(), int(QFont::Bold));
        appearance.setZoom(1.0);
    }

    void liteHasNoBadgeAndNamesItself() {
        BrandLockup lite(BrandLockup::Edition::Lite);
        BrandLockup pro(BrandLockup::Edition::Pro);
        QVERIFY(lite.badgeRect().isNull());
        QVERIFY(lite.sizeHint().width() < pro.sizeHint().width());
        QCOMPARE(lite.accessibleName(), QStringLiteral("ketPlus"));
        QCOMPARE(pro.accessibleName(), QStringLiteral("ketPlus Pro"));
    }

    void coloursFollowTheTheme() {
        auto& appearance = ket::Appearance::instance();
        BrandLockup lockup(BrandLockup::Edition::Pro);
        for (const auto mode : {ket::Appearance::Mode::Light, ket::Appearance::Mode::Dark}) {
            appearance.setMode(mode);
            QCOMPARE(lockup.brandColor(), ket::Tokens::color(u"accent-text", appearance.theme()));
            QCOMPARE(lockup.textColor(), ket::Tokens::color(u"text-main", appearance.theme()));
        }
        appearance.setMode(ket::Appearance::Mode::Light);
    }

    void rendersTheMarkAndWordmark() {
        BrandLockup lockup(BrandLockup::Edition::Pro);
        lockup.resize(lockup.sizeHint());
        const QImage image = lockup.grab().toImage();
        const QPoint markCentre = lockup.markRect().center().toPoint() * image.devicePixelRatio();
        QVERIFY2(qAlpha(image.pixel(markCentre)) > 200, "the mark is drawn");
    }

    void aboutDialogShowsTheLockupAndVersion() {
        QCoreApplication::setApplicationVersion(QStringLiteral("9.8.7"));
        QWidget window;
        window.resize(600, 400);
        window.show();
        ketplus::showAboutDialog(&window, BrandLockup::Edition::Pro);
        auto* dialog = window.findChild<QWidget*>(QStringLiteral("aboutDialog"));
        QVERIFY(dialog);
        auto* lockup = dialog->findChild<BrandLockup*>();
        QVERIFY(lockup);
        QCOMPARE(lockup->edition(), BrandLockup::Edition::Pro);
        QCOMPARE(dialog->findChild<QLabel*>(QStringLiteral("aboutVersion"))->text(),
                 QStringLiteral("Version 9.8.7"));
        // Reopening raises the same dialog instead of stacking another.
        ketplus::showAboutDialog(&window, BrandLockup::Edition::Pro);
        QCOMPARE(window.findChildren<QWidget*>(QStringLiteral("aboutDialog")).size(), 1);
    }
};

QTEST_MAIN(BrandLockupTest)
#include "BrandLockupTest.moc"
