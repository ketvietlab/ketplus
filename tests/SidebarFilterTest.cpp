// Synthetic Qt fixture; settings and paths are isolated from the installed application.
#include "KetAppearance.h"
#include "KetSidebarTree.h"
#include "KetStyle.h"
#include "app/MainWindow.h"
#include "app/SpotlightDialog.h"
#include "ui/InterfaceZoom.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"
#include "workspace/SidebarControls.h"
#include "workspace/SidebarFilterText.h"
#include "workspace/SidebarRepositoryList.h"
#include <QAction>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <memory>
using namespace ketplus;
namespace {
QVariantMap conversation(const QString& id, const QString& title, bool selected = false) {
    return {
        {"id", id},        {"title", title}, {"path", "/synthetic/work"}, {"selected", selected},
        {"started", true}, {"seen", true},   {"lastTurnStartedAt", 10}};
}
QVector<SidebarRepository> fixture() {
    QVariantList rows;
    for (int i = 0; i < 7; ++i)
        rows << conversation(QString::number(i),
                             i == 6 ? "Hidden Needle" : "Conversation " + QString::number(i));
    return {{.path = "/synthetic/alpha",
             .name = "Alpha",
             .active = true,
             .conversations = {conversation("active", "Current discussion", true)}},
            {.path = "/synthetic/tools/beta",
             .name = "Beta with a deliberately very long repository name",
             .conversations = rows,
             .teams = {{.id = "7",
                        .name = "Delivery",
                        .members = {conversation("lead", "Review Needle"),
                                    conversation("dev", "Other work")}}}},
            {.path = "/synthetic/empty", .name = "Empty Repo"}};
}
QStringList shownGroups(const SidebarRepositoryList& list) {
    QStringList paths;
    for (auto* group : list.findChildren<QWidget*>("productRepositoryGroup"))
        if (group->isVisible())
            paths << group->property("repositoryPath").toString();
    return paths;
}
QStringList shownRows(const SidebarRepositoryList& list) {
    QStringList ids;
    for (auto* row : list.findChildren<ket::SidebarRow*>("productConversationButton"))
        if (row->isVisible())
            ids << row->property("conversationId").toString();
    return ids;
}
} // namespace
class SidebarFilterTest final : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> settings_;
    QLocale originalLocale_;
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
    }
    void init() {
        settings_ = std::make_unique<QTemporaryDir>();
        QVERIFY(settings_->isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_->path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings_->path());
        QLocale::setDefault(QLocale("en_US"));
        ket::Appearance::instance().setMode(ket::Appearance::Mode::Light);
        ket::Appearance::instance().setZoom(1.0);
    }
    void cleanup() { QLocale::setDefault(originalLocale_); }
    void titleMatchKeepsParentsAndBypassesPreviewAndFold() {
        QSettings().setValue("sidebar/collapsedRepositories", QStringList{"/synthetic/tools/beta"});
        SidebarRepositoryList list;
        list.resize(240, 600);
        list.setRepositories(fixture());
        list.show();
        QTest::qWait(1);
        QVERIFY(!shownRows(list).contains("6"));
        const auto folds = QSettings().value("sidebar/collapsedRepositories");
        list.setFilterText("nEeDlE");
        QCOMPARE(shownGroups(list), QStringList{"/synthetic/tools/beta"});
        QCOMPARE(shownRows(list), (QStringList{"lead", "6"}));
        QCOMPARE(QSettings().value("sidebar/collapsedRepositories"), folds);
        list.setFilterText({});
        QVERIFY(!shownRows(list).contains("6"));
        QCOMPARE(shownGroups(list).size(), 3);
        QCOMPARE(QSettings().value("sidebar/collapsedRepositories"), folds);
    }
    void repositoryNameAndPathShowWholeGroupEmptyIsHonest() {
        SidebarRepositoryList list;
        list.resize(240, 600);
        list.setRepositories(fixture());
        list.show();
        list.setFilterText("  TOOLS/BETA  ");
        QCOMPARE(shownGroups(list), QStringList{"/synthetic/tools/beta"});
        QCOMPARE(shownRows(list).size(), 9);
        list.setFilterText("alpha");
        QCOMPARE(shownRows(list), QStringList{"active"});
        list.setFilterText("Empty Repo");
        QCOMPARE(shownGroups(list), QStringList{"/synthetic/empty"});
        auto* empty = list.findChild<QLabel*>("sidebarFilterEmpty");
        QVERIFY(empty);
        QVERIFY(!empty->isVisible());
        list.setFilterText("missing transcript contents");
        QVERIFY(shownGroups(list).isEmpty());
        QVERIFY(empty->isVisible());
        list.setFilterText({});
        QVERIFY(!empty->isVisible());
        QCOMPARE(shownGroups(list).size(), 3);
    }
    void updatesKeepQuerySelectionAndOrder_data() {
        QTest::addColumn<bool>("remote");
        QTest::newRow("local") << false;
        QTest::newRow("selected-machine") << true;
    }
    void updatesKeepQuerySelectionAndOrder() {
        QFETCH(bool, remote);
        ActivityBar sidebar;
        sidebar.setRemoteMachineSelected(remote);
        auto data = fixture();
        sidebar.setRepositories(data);
        sidebar.show();
        auto* input = sidebar.findChild<QLineEdit*>("sidebarRepositoryFilter");
        QVERIFY(input);
        QSignalSpy activated(&sidebar, &ActivityBar::conversationRequested);
        QSignalSpy repository(&sidebar, &ActivityBar::repositoryActivationRequested);
        input->setText("Needle");
        const auto before = shownGroups(*sidebar.repositoryList());
        auto row = data[1].conversations[6].toMap();
        row["lastTurnStartedAt"] = 99999;
        row["running"] = true;
        data[1].conversations[6] = row;
        sidebar.setRepositories(data);
        QCOMPARE(input->text(), QStringLiteral("Needle"));
        QCOMPARE(shownGroups(*sidebar.repositoryList()), before);
        QCOMPARE(activated.count(), 0);
        QCOMPARE(repository.count(), 0);
        row["title"] = "Renamed away";
        data[1].conversations[6] = row;
        sidebar.setRepositories(data);
        QCOMPARE(shownRows(*sidebar.repositoryList()), QStringList{"lead"});
        input->clear();
        QCOMPARE(shownGroups(*sidebar.repositoryList()),
                 (QStringList{"/synthetic/alpha", "/synthetic/tools/beta", "/synthetic/empty"}));
        auto* selected = sidebar.findChild<ket::SidebarRow*>("productConversationButton");
        QVERIFY(selected && selected->property("selectedConversation").toBool());
        QCOMPARE(selected->property("conversationId").toString(), QString("active"));
        QVERIFY(selected->isChecked());
        QCOMPARE(activated.count(), 0);
    }
    void historyMatchesMaterializeWithoutBuildingUnmatchedHistory() {
        SidebarRepositoryList list;
        list.resize(240, 600);
        auto data = fixture();
        data[1].teams.clear();
        data[1].conversations.clear();
        for (int i = 0; i < 2500; ++i)
            data[1].conversations << conversation(QString::number(i),
                                                  i == 2499 ? "Deep history match"
                                                            : "Conversation " + QString::number(i));
        list.setRepositories(data);
        list.show();
        QCOMPARE(list.findChildren<ket::SidebarRow*>("productConversationButton").size(), 6);
        list.setFilterText("deep history");
        QCOMPARE(shownRows(list), QStringList{"2499"});
        QCOMPARE(list.findChildren<ket::SidebarRow*>("productConversationButton").size(), 1);
        QPointer<ket::SidebarRow> kept =
            list.findChild<ket::SidebarRow*>("productConversationButton");
        auto hidden = data[1].conversations[100].toMap();
        hidden["title"] = "Unrelated title changed";
        data[1].conversations[100] = hidden;
        list.setRepositories(data);
        QVERIFY(kept);
        QCOMPARE(shownRows(list), QStringList{"2499"});
        list.setFilterText({});
        QCOMPARE(list.findChildren<ket::SidebarRow*>("productConversationButton").size(), 6);
        QCOMPARE(shownGroups(list).size(), 3);
    }
    void filteringDoesNotRewritePinsOrOrder() {
        QTemporaryDir dirs;
        QVERIFY(dirs.isValid());
        const QString alpha = QDir(dirs.path()).filePath("alpha");
        const QString beta = QDir(dirs.path()).filePath("beta");
        QVERIFY(QDir().mkpath(alpha) && QDir().mkpath(beta));
        QVector<SidebarRepository> data{{.path = alpha, .name = "Alpha", .pinnable = true},
                                        {.path = beta, .name = "Beta", .pinnable = true}};
        SidebarRepositoryList list;
        list.setRepositories(data);
        list.show();
        list.setRepositoryPinned(beta, true);
        const auto saved = QSettings().value("sidebar/pinnedRepositories");
        QCOMPARE(shownGroups(list), (QStringList{beta, alpha}));
        list.setFilterText("Alpha");
        QCOMPARE(shownGroups(list), QStringList{alpha});
        data[0].conversations = {conversation("new", "New running update")};
        list.setRepositories(data);
        list.setFilterText({});
        QCOMPARE(shownGroups(list), (QStringList{beta, alpha}));
        QCOMPARE(QSettings().value("sidebar/pinnedRepositories"), saved);
    }
    void keyboardKeepsGlobalSearchSeparate() {
        ThemeManager theme(*qApp);
        MainWindow window(theme);
        window.resize(1100, 720);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* field = window.findChild<QLineEdit*>("sidebarRepositoryFilter");
        auto* focus = window.findChild<QAction*>("sidebarFilterAction");
        auto* global = window.findChild<QAction*>("goToFileAction");
        QVERIFY(field && focus && global);
        QCOMPARE(focus->shortcut(), SidebarFilterField::shortcut());
        QCOMPARE(global->shortcut(), QKeySequence("Ctrl+P"));
        QSignalSpy opened(global, &QAction::triggered);
        QTest::keyClick(&window, Qt::Key_K, Qt::ControlModifier | Qt::AltModifier);
        QTRY_VERIFY(field->hasFocus());
        QTest::keyClicks(field, "Needle");
        QTest::keyClick(field, Qt::Key_Return);
        QVERIFY(window.findChildren<SpotlightDialog*>().isEmpty());
        QCOMPARE(opened.count(), 0);
        QTest::keyClick(field, Qt::Key_Escape);
        QVERIFY(field->text().isEmpty());
        QTest::keyClick(field, Qt::Key_P, Qt::ControlModifier);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_COMPARE(window.findChildren<SpotlightDialog*>().size(), 1);
        window.findChild<SpotlightDialog*>()->close();
        bool chord = false;
        for (auto* action : window.findChildren<QAction*>())
            chord = chord || action->shortcut() == QKeySequence("Ctrl+K, Ctrl+O");
        QVERIFY(chord);
    }
    void renderedField_data() {
        QTest::addColumn<QString>("locale");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("width");
        QTest::addColumn<double>("zoom");
        QTest::newRow("en-light") << QString("en_US") << false << 240 << 1.0;
        QTest::newRow("vi-dark-narrow") << QString("vi_VN") << true << 180 << 1.0;
        QTest::newRow("en-dark-wide") << QString("en_US") << true << 360 << 1.1;
        QTest::newRow("vi-light-narrow-zoom") << QString("vi_VN") << false << 180 << 1.25;
    }
    void liveAppearanceAndClearKeepTheFilterUsable() {
        QLocale::setDefault(QLocale("vi_VN"));
        ThemeManager theme(*qApp);
        ActivityBar sidebar;
        connect(&theme, &ThemeManager::themeChanged, &sidebar,
                [&] { sidebar.applyPalette(theme.palette(), theme.interfaceZoomPercent()); });
        sidebar.applyPalette(theme.palette(), theme.interfaceZoomPercent());
        sidebar.setSidebarWidth(180);
        sidebar.setRepositories(fixture());
        sidebar.show();
        auto* field = sidebar.findChild<QLineEdit*>("sidebarRepositoryFilter");
        auto* wrapper = sidebar.findChild<SidebarFilterField*>();
        QVERIFY(field && wrapper);
        QTest::mouseClick(field, Qt::LeftButton);
        QTest::keyClicks(field, "Needle");
        for (auto mode :
             {ThemeManager::Mode::Dark, ThemeManager::Mode::Light, ThemeManager::Mode::System}) {
            theme.setMode(mode);
            for (int percent : InterfaceZoomMetrics::steps()) {
                theme.setInterfaceScale(14, percent);
                QTRY_COMPARE(wrapper->height(), wrapper->sizeHint().height());
                QCOMPARE(field->geometry(), wrapper->rect());
                QCOMPARE(field->text(), QString("Needle"));
                QCOMPARE(shownRows(*sidebar.repositoryList()).size(), 2);
            }
        }
        auto* clear = field->findChild<QToolButton*>();
        QVERIFY(clear);
        QTRY_VERIFY(clear->isVisible());
        QVERIFY(field->rect().contains(clear->geometry()));
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_VERIFY(field->text().isEmpty());
        QTRY_VERIFY(!clear->isVisible());
        QCOMPARE(shownGroups(*sidebar.repositoryList()).size(), 3);
        field->clearFocus();
        const auto capture = qEnvironmentVariable("KETPLUS_SIDEBAR_FILTER_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(sidebar.grab().save(QDir(capture).filePath("vi-placeholder-live-zoom.png")));
        field->setText("no synthetic match");
        auto* empty = sidebar.findChild<QLabel*>("sidebarFilterEmpty");
        QTRY_VERIFY(empty && empty->isVisible());
        QVERIFY(empty->height() >= empty->heightForWidth(empty->width()));
        QCOMPARE(shownGroups(*sidebar.repositoryList()).size(), 0);
        if (!capture.isEmpty())
            QVERIFY(sidebar.grab().save(QDir(capture).filePath("vi-empty-live-zoom.png")));
    }
    void renderedField() {
        QFETCH(QString, locale);
        QFETCH(bool, dark);
        QFETCH(int, width);
        QFETCH(double, zoom);
        QLocale::setDefault(QLocale(locale));
        ThemeManager theme(*qApp);
        theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        theme.setInterfaceScale(14, qRound(zoom * 100));
        ActivityBar sidebar;
        sidebar.applyPalette(theme.palette(), theme.interfaceZoomPercent());
        sidebar.setSidebarWidth(width);
        sidebar.setRepositories(fixture());
        sidebar.show();
        auto* field = sidebar.findChild<QLineEdit*>("sidebarRepositoryFilter");
        QVERIFY(field);
        QCOMPARE(field->accessibleName(), sidebar_filter::text(sidebar_filter::name));
        if (locale.startsWith("vi"))
            QVERIFY(field->accessibleName().startsWith(QString::fromUtf8("Lọc")));
        QVERIFY(field->toolTip().contains(
            SidebarFilterField::shortcut().toString(QKeySequence::NativeText)));
        QTest::mouseClick(field, Qt::LeftButton);
        QTest::keyClicks(field, "Needle");
        QTRY_COMPARE(shownRows(*sidebar.repositoryList()).size(), 2);
        QVERIFY(sidebar.rect().contains(field->mapTo(&sidebar, QPoint(0, 0))));
        QVERIFY(sidebar.rect().contains(
            field->mapTo(&sidebar, QPoint(field->width() - 1, field->height() - 1))));
        QVERIFY2(sidebar.repositoryList()->layoutProblems().isEmpty(),
                 qPrintable(sidebar.repositoryList()->layoutProblems().join("; ")));
        const auto capture = qEnvironmentVariable("KETPLUS_SIDEBAR_FILTER_CAPTURE");
        if (!capture.isEmpty()) {
            QDir().mkpath(capture);
            QVERIFY(sidebar.grab().save(
                QDir(capture).filePath(QString::fromLatin1(QTest::currentDataTag()) + ".png")));
        }
        QTest::keyClick(field, Qt::Key_Escape);
        QCOMPARE(shownGroups(*sidebar.repositoryList()).size(), 3);
    }
};
QTEST_MAIN(SidebarFilterTest)
#include "SidebarFilterTest.moc"
