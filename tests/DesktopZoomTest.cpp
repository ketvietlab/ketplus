#include "agent/AgentWorkspaceView.h"
#include "app/AppearanceSettings.h"
#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "ui/ChromeGeometry.h"
#include "ui/InterfaceFont.h"
#include "ui/InterfaceZoom.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"

#include "KetNavItem.h"
#include "KetProfile.h"
#include "KetSegmentedControl.h"
#include "KetSidebarTree.h"

#include "app/PixelSelect.h"
#include <QAbstractButton>
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFontInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QQuickItem>
#include <QQuickView>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>
#include <QToolTip>
#include <QtTest>

using namespace ketplus;
namespace {
void installFontOverlay(ThemeManager& theme) {
    InterfaceUiFont::apply(*qApp, theme.interfaceTextPixels());
    QObject::connect(&theme, &ThemeManager::themeChanged, &theme,
                     [&theme] { InterfaceUiFont::apply(*qApp, theme.interfaceTextPixels()); });
}
void openSettings(MainWindow& window) {
    auto* action = window.findChild<QAction*>(QStringLiteral("productSettingsAction"));
    QVERIFY(action);
    action->trigger();
}
// Settings changes apply as they are made (ADR 0103); there is no Save to press.
void expectApplied(MainWindow& window) {
    QVERIFY(window.findChild<QAbstractButton*>(QStringLiteral("settingsSaveButton")) == nullptr);
}
} // namespace

class DesktopZoomTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
  private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("KetPlusZoomTests"));
        QCoreApplication::setApplicationName(QStringLiteral("KetPlus"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        qputenv("KETPLUS_CONVERSATION_WORKTREES", settings_.filePath("worktrees.sqlite").toUtf8());
    }
    void init() { QSettings().clear(); }

    void fontAndGeometryFollowZoomWithoutCompounding_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void fontAndGeometryFollowZoomWithoutCompounding() {
        QFETCH(bool, dark);
        ThemeManager theme(*qApp);
        installFontOverlay(theme);
        theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        QWidget surface;
        auto* layout = new QHBoxLayout(&surface);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* bar = new ActivityBar(&surface);
        auto* agent = new AgentWorkspaceView(&surface);
        agent->setProductSidebarVisible(false);
        agent->setAgentAvailable(true);
        agent->setSessionState("Typography review", "Ready", false);
        agent->resetMessages(
            {QVariantMap{{"kind", "user"},
                         {"body", "Kiểm tra phông chữ Két Việt và khoảng cách giao diện."},
                         {"meta", "You"}},
             QVariantMap{{"kind", "assistant"},
                         {"body",
                          "## Giao diện Két Việt\n\nQuyền sở hữu, dữ liệu và cộng đồng — tiếng "
                          "Việt có dấu. "
                          "Nội dung dài tự xuống dòng trong hội thoại. Số tiền: 1.234.567 ₫.\n\n"
                          "- Small giữ giao diện gọn.\n- Medium giữ tỷ lệ thiết kế.\n"
                          "- Large tăng kích thước điều khiển và khoảng cách."},
                         {"meta", "KetPlus Agent"}}});
        layout->addWidget(bar);
        layout->addWidget(agent, 1);
        surface.resize(1180, 760);
        QLabel inherited(QStringLiteral("Inherited UI font"));
        inherited.show();
        surface.show();
        SidebarRepository repo;
        repo.name = "Desktop";
        repo.path = "/synthetic";
        const QString title = "Kiểm tra phông chữ và khoảng cách giao diện Két Việt";
        repo.conversations = {QVariantMap{{"id", "first"}, {"path", repo.path}, {"title", title}}};
        bar->setRepositories({repo});
        bar->setMode(ActivityBar::Mode::AgentWorkspace);
        const auto* nav = bar->findChild<QAbstractButton*>("agentWorkspaceActivityButton");
        QVERIFY(nav);
        const auto* root = agent->quickView()->rootObject();
        QVERIFY(root);
        auto* header = root->findChild<QQuickItem*>("agentWorkspaceHeader");
        auto* footer = root->findChild<QQuickItem*>("composerFooter");
        QVERIFY(header && footer);
        for (int zoom : {90, 100, 110, 125, 90, 110, 100}) {
            theme.setInterfaceScale(14, zoom);
            bar->applyPalette(theme.palette(), zoom);
            agent->applyTheme(theme.palette(), zoom);
            QTest::qWait(80);
            QCOMPARE(ket::Appearance::instance().zoom(), InterfaceZoomMetrics::factor(zoom));
            const auto px = [zoom](int n) { return InterfaceZoomMetrics::geometry(n, zoom); };
            QCOMPARE(inherited.font().pixelSize(), InterfaceZoomMetrics::scale(14, zoom));
            QCOMPARE(nav->height(), nav->sizeHint().height());
            QTRY_COMPARE(header->height(), px(44));
            QTRY_COMPARE(footer->height(), px(28));
            auto* row = bar->findChild<ket::SidebarRow*>("productConversationButton");
            QVERIFY(row);
            QCOMPARE(row->height(), row->sizeHint().height());
            QCOMPARE(row->text(), title);
            QVERIFY(row->recipe().height > 0);
            // Rows created after a zoom change inherit the same geometry.
            repo.conversations.append(QVariantMap{
                {"id", QString::number(zoom) + QString::number(repo.conversations.size())},
                {"path", repo.path},
                {"title", "New row"}});
            bar->setRepositories({repo});
            for (auto* newRow : bar->findChildren<QAbstractButton*>("productConversationButton"))
                QCOMPARE(newRow->height(), newRow->sizeHint().height());
            QTest::qWait(80); // settle rows created above before native captures
            const auto captureRoot = qEnvironmentVariable("KETPLUS_ZOOM_CAPTURE");
            const auto capture =
                captureRoot.isEmpty() ? QString{} : captureRoot + (dark ? "/dark" : "/light");
            if (!capture.isEmpty()) {
                QDir().mkpath(capture);
                QVERIFY(bar->grab().save(capture + QString("/sidebar-%1.png").arg(zoom)));
                // The mock's workspace/storage-warning: the journal banner above Update ready.
                bar->setStorageStatus(
                    {{"ok", true},
                     {"status", QVariantMap{{"warning", true},
                                            {"over_limit", true},
                                            {"database_page_bytes", 9'000'000'000LL},
                                            {"limit_bytes", 5'000'000'000LL}}}});
                bar->setPendingUpdate("1.8.0", true);
                QTest::qWait(40);
                QVERIFY(bar->grab().save(capture + QString("/sidebar-storage-%1.png").arg(zoom)));
                bar->setStorageStatus({{"ok", true}, {"status", QVariantMap{{"warning", false}}}});
                bar->setPendingUpdate({});
                QVERIFY(agent->quickView()->grabWindow().save(capture +
                                                              QString("/agent-%1.png").arg(zoom)));
            }
        }
    }

    void twoPixelGapsAndHairlinesStaySmall() {
        QWidget surface;
        auto* layout = new QHBoxLayout(&surface);
        chrome_geometry::margins(layout, 0, 1, 2, 8);
        chrome_geometry::spacing(layout, 2);
        chrome_geometry::addSpacing(layout, 12);
        for (int zoom : {90, 110, 125, 100, 90}) {
            chrome_geometry::apply(&surface, zoom);
            QCOMPARE(layout->contentsMargins().left(), 0);
            QCOMPARE(layout->contentsMargins().top(), 1);
            // Two pixels stay two up to 110%; 125% rounds them up like the mock's px().
            QCOMPARE(layout->contentsMargins().right(), zoom < 125 ? 2 : 3);
            QCOMPARE(layout->spacing(), zoom < 125 ? 2 : 3);
            QCOMPARE(layout->itemAt(0)->spacerItem()->sizeHint().width(),
                     InterfaceZoomMetrics::geometry(12, zoom));
        }
    }

    void tooltipsKeepTheKitSizeAtEveryInterfaceSize() {
        // ThemeManager scales compatibility QSS once; the font overlay preserves the
        // kit's tooltip recipe at each numeric size and after a theme change.
        ThemeManager theme(*qApp);
        installFontOverlay(theme);
        QWidget anchor;
        anchor.resize(80, 40);
        anchor.show();
        const auto shownPixels = [&anchor] {
            QToolTip::showText(anchor.mapToGlobal(QPoint(10, 10)), QStringLiteral("Tip"), &anchor);
            int pixels = -1;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (widget->inherits("QTipLabel") && widget->isVisible()) {
                    widget->ensurePolished();
                    pixels = QFontInfo(widget->font()).pixelSize();
                }
            }
            QToolTip::hideText();
            return pixels;
        };
        QCOMPARE(shownPixels(), tooltipFontPixels());
        theme.setInterfaceScale(16, 125);
        QCOMPARE(shownPixels(), tooltipFontPixels());
        theme.setMode(ThemeManager::Mode::Dark);
        QCOMPARE(shownPixels(), tooltipFontPixels());
        QVERIFY(tooltipFontPixels() < theme.interfaceTextPixels());
        theme.setInterfaceScale(14, 100);
    }

    void appearanceUpdatesAllWindowsAndMonoWithoutZoom() {
        ThemeManager theme(*qApp);
        installFontOverlay(theme);
        QTemporaryDir repo1, repo2;
        MainWindow first(theme), second(theme);
        first.openFolder(repo1.path());
        second.openFolder(repo2.path());
        first.show();
        second.show();
        first.findChild<QAbstractButton*>("agentWorkspaceActivityButton")->click();
        second.findChild<QAbstractButton*>("agentWorkspaceActivityButton")->click();
        auto* one = first.findChild<AgentWorkspaceView*>();
        auto* two = second.findChild<AgentWorkspaceView*>();
        QVERIFY(one && two);
        openSettings(first);
        auto* zoom = first.findChild<ket::SegmentedControl*>("interfaceZoom");
        QVERIFY(zoom);
        zoom->setCurrentIndex(int(InterfaceZoomMetrics::steps().indexOf(125)));
        expectApplied(first);
        const auto readingPixels = [](AgentWorkspaceView* view) {
            return view->quickView()
                ->rootObject()
                ->findChild<QQuickItem*>("conversationTitle")
                ->property("font")
                .value<QFont>()
                .pixelSize();
        };
        QTRY_COMPARE(readingPixels(one), InterfaceZoomMetrics::scale(
                                             qRound(ket::Profile::current().readingPixels), 125));
        QTRY_COMPARE(readingPixels(two), readingPixels(one));
        openSettings(second);
        auto* secondZoom = second.findChild<ket::SegmentedControl*>("interfaceZoom");
        QVERIFY(secondZoom);
        QCOMPARE(secondZoom->currentIndex(), int(InterfaceZoomMetrics::steps().indexOf(125)));
        auto* family = second.findChild<QComboBox*>("editorFontFamily");
        QVERIFY(family);
        family->addItem(QStringLiteral("Courier New"));
        family->setCurrentText(QStringLiteral("Courier New"));
        expectApplied(second);
        QTRY_COMPARE(one->monoFontFamily(), QStringLiteral("Courier New"));
        QTRY_COMPARE(two->monoFontFamily(), QStringLiteral("Courier New"));
        AgentWorkspaceView later;
        QCOMPARE(later.monoFontFamily(), QStringLiteral("Courier New"));
        // First window still has a form opened before the font change. Changing a
        // different field must not overwrite the newer font from the other window.
        QTest::qWait(50);
        first.findChild<ketplus::PixelSelect*>("editorFontSize")->setValue(17);
        expectApplied(first);
        QCOMPARE(AppearanceSettings::load().editor.fontFamily, QStringLiteral("Courier New"));
        QCOMPARE(AppearanceSettings::load().editor.fontSizePixels, 17);
    }
};
QTEST_MAIN(DesktopZoomTest)
#include "DesktopZoomTest.moc"
