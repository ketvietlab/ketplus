#include "agent/AgentWorkspaceView.h"
#include "app/AppearanceSettings.h"
#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "ui/ChromeGeometry.h"
#include "ui/InterfaceFont.h"
#include "ui/InterfaceZoom.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"

#include "KetProfile.h"
#include "KetStepSlider.h"

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
#include <QSpinBox>
#include <QTemporaryDir>
#include <QToolButton>
#include <QToolTip>
#include <QtTest>

using namespace ketplus;
namespace {
void installFontOverlay(ThemeManager& theme) {
    InterfaceUiFont::apply(*qApp, theme.interfaceFontSizePixels());
    QObject::connect(&theme, &ThemeManager::themeChanged, &theme,
                     [&theme] { InterfaceUiFont::apply(*qApp, theme.interfaceFontSizePixels()); });
}
void openSettings(MainWindow& window) {
    auto* action = window.findChild<QAction*>(QStringLiteral("productSettingsAction"));
    QVERIFY(action);
    action->trigger();
}
void saveSettings(MainWindow& window) {
    for (auto* button : window.findChildren<QAbstractButton*>()) {
        if (button->property("kvRole") == QStringLiteral("primaryAction")) {
            button->click();
            return;
        }
    }
    QFAIL("No settings save action");
}
QLabel* conversationLabel(ActivityBar& sidebar) {
    for (auto* label : sidebar.findChildren<QLabel*>())
        if (label->property("kvRole") == QStringLiteral("productConversationTitle") &&
            label->text() != QStringLiteral("New row"))
            return label;
    return nullptr;
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
        const auto* nav = bar->findChild<QToolButton*>("agentWorkspaceActivityButton");
        QVERIFY(nav);
        const auto* root = agent->quickView()->rootObject();
        QVERIFY(root);
        auto* header = root->findChild<QQuickItem*>("agentWorkspaceHeader");
        auto* footer = root->findChild<QQuickItem*>("composerFooter");
        QVERIFY(header && footer);
        for (int pixels : {12, 14, 16, 18, 12, 16, 14}) {
            theme.setInterfaceFontSizePixels(pixels);
            bar->applyPalette(theme.palette(), pixels);
            agent->applyTheme(theme.palette(), pixels);
            QTest::qWait(80);
            QCOMPARE(ket::Appearance::instance().zoom(), InterfaceZoomMetrics::factor(pixels));
            const auto px = [pixels](int n) { return InterfaceZoomMetrics::geometry(n, pixels); };
            QCOMPARE(inherited.font().pixelSize(), pixels);
            QCOMPARE(nav->height(), px(32));
            QTRY_COMPARE(header->height(), px(44));
            QTRY_COMPARE(footer->height(), px(28));
            auto* row = bar->findChild<QToolButton*>("productConversationButton");
            QVERIFY(row && row->layout());
            QCOMPARE(row->height(), px(36));
            QCOMPARE(row->layout()->contentsMargins().left(), px(8));
            QCOMPARE(row->layout()->spacing(), px(8));
            auto* label = conversationLabel(*bar);
            QVERIFY(label);
            QCOMPARE(label->text(),
                     label->fontMetrics().elidedText(title, Qt::ElideRight, label->width()));
            QVERIFY(label->fontMetrics().horizontalAdvance(label->text()) <= label->width());
            // Rows created after a zoom change inherit the same geometry.
            repo.conversations.append(QVariantMap{
                {"id", QString::number(pixels) + QString::number(repo.conversations.size())},
                {"path", repo.path},
                {"title", "New row"}});
            bar->setRepositories({repo});
            for (auto* newRow : bar->findChildren<QToolButton*>("productConversationButton"))
                QCOMPARE(newRow->height(), px(36));
            QTest::qWait(80); // settle rows created above before native captures
            const auto captureRoot = qEnvironmentVariable("KETPLUS_ZOOM_CAPTURE");
            const auto capture =
                captureRoot.isEmpty() ? QString{} : captureRoot + (dark ? "/dark" : "/light");
            if (!capture.isEmpty()) {
                QDir().mkpath(capture);
                QVERIFY(bar->grab().save(capture + QString("/sidebar-%1.png").arg(pixels)));
                QVERIFY(agent->quickView()->grabWindow().save(
                    capture + QString("/agent-%1.png").arg(pixels)));
            }
        }
    }

    void twoPixelGapsAndHairlinesStaySmall() {
        QWidget surface;
        auto* layout = new QHBoxLayout(&surface);
        chrome_geometry::margins(layout, 0, 1, 2, 8);
        chrome_geometry::spacing(layout, 2);
        chrome_geometry::addSpacing(layout, 12);
        for (int pixels : {12, 16, 14, 12}) {
            chrome_geometry::apply(&surface, pixels);
            QCOMPARE(layout->contentsMargins().left(), 0);
            QCOMPARE(layout->contentsMargins().top(), 1);
            QCOMPARE(layout->contentsMargins().right(), 2);
            QCOMPARE(layout->spacing(), 2);
            QCOMPARE(layout->itemAt(0)->spacerItem()->sizeHint().width(),
                     InterfaceZoomMetrics::geometry(12, pixels));
        }
    }

    void tooltipsKeepTheKitSizeAtEveryInterfaceSize() {
        // InterfaceUiFont rewrites and scales the stylesheet; the tooltip rule keeps the
        // kit's recipe size at Medium and Large and after a theme change.
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
        theme.setInterfaceFontSizePixels(18);
        QCOMPARE(shownPixels(), tooltipFontPixels());
        theme.setMode(ThemeManager::Mode::Dark);
        QCOMPARE(shownPixels(), tooltipFontPixels());
        QVERIFY(tooltipFontPixels() < 18);
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
        first.findChild<QToolButton*>("agentWorkspaceActivityButton")->click();
        second.findChild<QToolButton*>("agentWorkspaceActivityButton")->click();
        auto* one = first.findChild<AgentWorkspaceView*>();
        auto* two = second.findChild<AgentWorkspaceView*>();
        QVERIFY(one && two);
        openSettings(first);
        auto* zoom = first.findChild<ket::StepSlider*>("interfaceZoomSteps");
        QVERIFY(zoom);
        zoom->setCurrentIndex(16 - AppearanceSettings::minimumInterfaceFontSizePixels);
        saveSettings(first);
        QTRY_COMPARE(
            one->tokens().value("reading").toInt(),
            InterfaceZoomMetrics::scale(qRound(ket::Profile::current().readingPixels), 16));
        QTRY_COMPARE(two->tokens().value("reading").toInt(),
                     one->tokens().value("reading").toInt());
        openSettings(second);
        auto* secondZoom = second.findChild<ket::StepSlider*>("interfaceZoomSteps");
        QVERIFY(secondZoom);
        QCOMPARE(secondZoom->currentIndex(),
                 16 - AppearanceSettings::minimumInterfaceFontSizePixels);
        auto* family = second.findChild<QComboBox*>("editorFontFamily");
        QVERIFY(family);
        family->addItem(QStringLiteral("Courier New"));
        family->setCurrentText(QStringLiteral("Courier New"));
        saveSettings(second);
        QTRY_COMPARE(one->tokens().value("monoFont").toString(), QStringLiteral("Courier New"));
        QTRY_COMPARE(two->tokens().value("monoFont").toString(), QStringLiteral("Courier New"));
        AgentWorkspaceView later;
        QCOMPARE(later.tokens().value("monoFont").toString(), QStringLiteral("Courier New"));
        // First window still has a form opened before the font change. Saving a
        // different field must not overwrite the newer font from the other window.
        QTest::qWait(50);
        first.findChild<QSpinBox*>("editorFontSize")->setValue(17);
        first.findChild<QSpinBox*>("previewFontSize")->setValue(19);
        saveSettings(first);
        QCOMPARE(AppearanceSettings::load().editor.fontFamily, QStringLiteral("Courier New"));
        QCOMPARE(AppearanceSettings::load().preview.fontSizePixels, 19);
        QCOMPARE(AppearanceSettings::load().editor.fontSizePixels, 17);
    }
};
QTEST_MAIN(DesktopZoomTest)
#include "DesktopZoomTest.moc"
