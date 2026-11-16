#include "ui/BrandLockup.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetTokens.h"
#include "ui/AboutDialog.h"
#include "ui/InterfaceZoom.h"
#include "ui/KetFonts.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>

using ketplus::BrandLockup;

// The approved geometry from mockups/qt/qml/parts/BrandLockup.qml.
class BrandLockupTest : public QObject {
    Q_OBJECT
    QList<QUrl> openedUrls_;

  public slots:
    void recordUrl(const QUrl& url) { openedUrls_.append(url); }

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

    void installedAboutLayouts() {
        using Edition = BrandLockup::Edition;
        struct Layout {
            Edition edition;
            QString application;
            QString licenses;
        };
        const QList<Layout> layouts{
            {Edition::Lite, "Lite.app/Contents/MacOS", "Lite.app/Contents/Resources/licenses"},
            {Edition::Pro, "Pro.app/Contents/MacOS", "Pro.app/Contents/Resources/licenses"},
            {Edition::Lite, "bin", "share/licenses/ketplus-cm"},
            {Edition::Pro, "bin", "share/licenses/ketplus"},
            {Edition::Lite, ".", "licenses"},
            {Edition::Pro, ".", "licenses"}};
        for (const auto& layout : layouts) {
            QTemporaryDir temporary;
            QVERIFY(temporary.isValid());
            QDir root(temporary.path());
            QVERIFY(root.mkpath(layout.application));
            QVERIFY(root.mkpath(layout.licenses));
            for (const auto& name : {"LICENSE", "THIRD_PARTY_NOTICES.md"}) {
                QFile file(root.filePath(layout.licenses + "/" + name));
                QVERIFY(file.open(QIODevice::WriteOnly));
                file.write("Synthetic license fixture\n");
            }
            const QString revision(40, QLatin1Char('a'));
            const auto information = ketplus::aboutInformation(
                layout.edition, root.filePath(layout.application), revision);
            QCOMPARE(information.revision, revision);
            QCOMPARE(information.source.toString(),
                     "https://github.com/ketvietlab/ketplus/tree/" + revision);
            QCOMPARE(QDir(information.licenses.toLocalFile()).absolutePath(),
                     QDir(root.filePath(layout.licenses)).absolutePath());
            QVERIFY(QFileInfo::exists(information.notices.toLocalFile()));
        }
    }

    void aboutSourceAndNoticeActionsAndMissingFiles() {
        QWidget window;
        window.resize(700, 640);
        window.show();
        const QString revision(40, QLatin1Char('a'));
        ketplus::AboutInformation information{
            revision, QUrl("https://github.com/ketvietlab/ketplus/tree/" + revision),
            QUrl::fromLocalFile("/synthetic/licenses/THIRD_PARTY_NOTICES.md"),
            QUrl::fromLocalFile("/synthetic/licenses")};
        QDesktopServices::setUrlHandler("https", this, "recordUrl");
        QDesktopServices::setUrlHandler("file", this, "recordUrl");
        openedUrls_.clear();
        ketplus::showAboutDialog(&window, BrandLockup::Edition::Pro, information);
        auto* dialog = window.findChild<QWidget*>("aboutDialog");
        QVERIFY(dialog);
        for (const auto& name : {"aboutSource", "aboutNotices", "aboutLicenses"}) {
            auto* button = dialog->findChild<ket::Button*>(name);
            QVERIFY(button && button->isEnabled());
            button->setFocus();
            QTest::keyClick(button, Qt::Key_Space);
        }
        QCOMPARE(openedUrls_,
                 QList<QUrl>({information.source, information.notices, information.licenses}));
        QDesktopServices::unsetUrlHandler("https");
        QDesktopServices::unsetUrlHandler("file");
        delete dialog;
        QTemporaryDir empty;
        information =
            ketplus::aboutInformation(BrandLockup::Edition::Lite, empty.path(), "unknown");
        ketplus::showAboutDialog(&window, BrandLockup::Edition::Lite, information);
        dialog = window.findChild<QWidget*>("aboutDialog");
        QVERIFY(dialog);
        for (const auto& name : {"aboutSource", "aboutNotices", "aboutLicenses"})
            QVERIFY(!dialog->findChild<ket::Button*>(name)->isEnabled());
        QVERIFY(!dialog->findChild<QLabel*>("aboutAccessStatus")->text().isEmpty());
        information = ketplus::aboutInformation(BrandLockup::Edition::Lite, empty.path(),
                                                revision + "-dirty");
        QVERIFY(!information.source.isEmpty());
        delete dialog;
        ketplus::showAboutDialog(&window, BrandLockup::Edition::Lite, information);
        QVERIFY(window.findChild<QLabel*>("aboutAccessStatus")->text().contains("Uncommitted"));
    }

    void aboutLayoutAndCaptures() {
        QCoreApplication::setApplicationVersion(QStringLiteral("9.8.7"));
        auto& appearance = ket::Appearance::instance();
        QWidget window;
        window.resize(680, 420);
        window.show();
        const QString revision(40, QLatin1Char('a'));
        const ketplus::AboutInformation information{
            revision, QUrl("https://github.com/ketvietlab/ketplus/tree/" + revision), {}, {}};
        ketplus::showAboutDialog(&window, BrandLockup::Edition::Pro, information);
        for (const auto mode : {ket::Appearance::Mode::Light, ket::Appearance::Mode::Dark}) {
            appearance.setMode(mode);
            for (const auto percent : ketplus::InterfaceZoomMetrics::steps()) {
                const auto zoom = percent / 100.0;
                appearance.setZoom(zoom);
                QCoreApplication::processEvents();
                auto* dialog = window.findChild<QWidget*>("aboutDialog");
                QCOMPARE(dialog->findChild<QLabel*>("aboutRevision")
                             ->palette()
                             .color(QPalette::WindowText),
                         ket::Tokens::color(u"text-main", appearance.theme()));
                const auto buttons = dialog->findChildren<ket::Button*>();
                auto* scroll = dialog->findChild<QScrollArea*>("aboutScrollArea");
                for (auto* button : buttons) {
                    if (scroll->widget()->isAncestorOf(button)) {
                        scroll->ensureWidgetVisible(button);
                        QCoreApplication::processEvents();
                        QVERIFY(button->height() >= button->sizeHint().height());
                    }
                    QVERIFY(button->width() >= button->minimumSizeHint().width());
                    QVERIFY(window.rect().contains(
                        QRect(button->mapTo(&window, QPoint()), button->size())));
                }
                const QString output = qEnvironmentVariable("KETPLUS_ABOUT_CAPTURE_DIR");
                if (!output.isEmpty() && (percent == 100 || percent == 125)) {
                    window.resize(700, 720);
                    scroll->verticalScrollBar()->setValue(0);
                    QCoreApplication::processEvents();
                    QVERIFY(scroll->viewport()->height() >= scroll->widget()->sizeHint().height());
                    QVERIFY(QDir().mkpath(output));
                    const QString name =
                        QString("about-%1-%2.png")
                            .arg(mode == ket::Appearance::Mode::Dark ? "dark" : "light")
                            .arg(zoom);
                    QVERIFY(window.grab().save(QDir(output).filePath(name)));
                    window.resize(680, 420);
                }
            }
        }
        appearance.setZoom(1.0);
        appearance.setMode(ket::Appearance::Mode::Light);
    }
};

QTEST_MAIN(BrandLockupTest)
#include "BrandLockupTest.moc"
