// The KétPlus (Lite) window against the Lite mock (mockups/qt, --edition=lite).
#include "app/MainWindow.h"
#include "app/ReviewCapture.h"
#include "terminal/TerminalPanel.h"
#include "ui/Theme.h"
#include "workspace/ExplorerPanel.h"

#include "KetAppearance.h"
#include "KetChrome.h"
#include "KetFileRow.h"
#include "KetTokens.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMenuBar>
#include <QProcess>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

namespace {

void write(const QString& path, const QByteArray& text) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(text);
}

QAction* viewAction(QMainWindow& window, const QString& label) {
    for (auto* menu : window.menuBar()->actions()) {
        if (menu->text().remove('&') != QStringLiteral("View") || menu->menu() == nullptr)
            continue;
        for (auto* action : menu->menu()->actions()) {
            if (action->text().remove('&') == label)
                return action;
        }
    }
    return nullptr;
}

QAbstractButton* activityButton(QMainWindow& window, const QString& name) {
    auto* rail = window.findChild<QWidget*>(QStringLiteral("liteActivityBar"));
    for (auto* button : rail->findChildren<QAbstractButton*>()) {
        if (button->accessibleName() == name)
            return button;
    }
    return nullptr;
}

// The 2 px accent rule at the left edge marks the panel on screen.
bool marked(QAbstractButton* button) {
    const auto accent = ket::Tokens::color(u"accent", ket::Appearance::instance().theme());
    return button->grab().toImage().pixelColor(0, button->height() / 2) == accent;
}

class PaintCounter final : public QObject {
  public:
    int paints = 0;

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Paint)
            ++paints;
        return false;
    }
};

} // namespace

class LiteWindowTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        QVERIFY(workspace_.isValid());
        QVERIFY(QDir(workspace_.path()).mkpath(QStringLiteral("docs")));
        write(workspace_.filePath(QStringLiteral("docs/guide.md")), "# Guide\n");
        write(workspace_.filePath(QStringLiteral("main.cpp")), "int main() {}\n");
    }

    void sidePanelStartsClosedAndItsButtonTogglesIt() {
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace_.path());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* tree = window.findChild<QTreeView*>(QStringLiteral("explorerTree"));
        QVERIFY(!tree->isVisibleTo(&window));
        auto* explorer = activityButton(window, QStringLiteral("Explorer"));
        QVERIFY(!marked(explorer));
        explorer->click();
        QVERIFY(tree->isVisibleTo(&window));
        QVERIFY(marked(explorer));
        explorer->click();
        QVERIFY(!tree->isVisibleTo(&window));
        QVERIFY(!marked(explorer));
        // Opening another folder keeps an open Explorer open.
        explorer->click();
        window.openFolder(workspace_.filePath(QStringLiteral("docs")));
        QVERIFY(tree->isVisibleTo(&window));
    }

    void explorerShowsTheFolderAsItsRootRow() {
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace_.path());
        viewAction(window, QStringLiteral("Explorer"))->trigger();
        auto* root = window.findChild<ket::FileRow*>(QStringLiteral("explorerRoot"));
        QVERIFY(root != nullptr);
        QCOMPARE(root->text(), QFileInfo(workspace_.path()).fileName().toUpper());
        QVERIFY(root->isChecked());
        auto* tree = window.findChild<QTreeView*>(QStringLiteral("explorerTree"));
        QVERIFY(tree->isVisibleTo(&window));
        root->click();
        QVERIFY(!tree->isVisibleTo(&window));
        root->click();
        QVERIFY(tree->isVisibleTo(&window));

        // The mock's header: EXPLORER with Refresh; the other actions are in the context menu.
        auto* header = root->parentWidget()->findChild<ket::PaneHeader*>();
        QCOMPARE(header->title(), QStringLiteral("EXPLORER"));
        QStringList actions;
        for (auto* button : header->findChildren<QAbstractButton*>())
            actions.append(button->accessibleName());
        QCOMPARE(actions, QStringList{QStringLiteral("Refresh Explorer")});
    }

    void activityBarMarksThePanelOnScreen() {
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace_.path());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        viewAction(window, QStringLiteral("Explorer"))->trigger();
        auto* explorer = activityButton(window, QStringLiteral("Explorer"));
        auto* search = activityButton(window, QStringLiteral("Search in Files"));
        QVERIFY(explorer && search);
        QVERIFY(marked(explorer));
        QVERIFY(!marked(search));
        // The mock's 248 px Explorer column at the interface zoom.
        QCOMPARE(window.findChild<QTreeView*>(QStringLiteral("explorerTree"))->window(), &window);
        QCOMPARE(window.findChild<ket::FileRow*>(QStringLiteral("explorerRoot"))->width(),
                 qRound(ket::Appearance::instance().scaled(248)));

        // Switching panels sets the actions under signal blockers; the buttons still repaint.
        PaintCounter counter;
        explorer->installEventFilter(&counter);
        viewAction(window, QStringLiteral("Search in Files"))->trigger();
        QTRY_VERIFY(counter.paints > 0);
        QVERIFY(!marked(explorer));
        QVERIFY(marked(search));
        // Assistive technology reads the panel on screen as the checked button.
        QVERIFY(search->isCheckable() && search->isChecked() && !explorer->isChecked());
        search->click();
        QVERIFY(!search->isChecked());
        QVERIFY(!viewAction(window, QStringLiteral("Search in Files"))->isChecked());
    }

    void sidePanelWidthFollowsTheZoom() {
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.resize(1280, 820);
        window.openFolder(workspace_.path());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* explorer = window.findChild<ketplus::ExplorerPanel*>();
        // A width the user dragged scales with the zoom, too.
        auto* split = qobject_cast<QSplitter*>(explorer->parentWidget());
        QVERIFY(split != nullptr);
        auto sizes = split->sizes();
        const int panel = split->indexOf(explorer);
        const int editor =
            split->indexOf(window.findChild<QWidget*>(QStringLiteral("editorColumn")));
        sizes[editor] += sizes[panel] - 300;
        sizes[panel] = 300;
        split->setSizes(sizes);
        QCOMPARE(explorer->width(), 300);
        ket::Appearance::instance().setZoom(1.25);
        QTRY_COMPARE(explorer->width(), 375);
        ket::Appearance::instance().setZoom(1.0);
        QTRY_COMPARE(explorer->width(), 300);
    }

    void revealOpensACollapsedRoot() {
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace_.path());
        auto* root = window.findChild<ket::FileRow*>(QStringLiteral("explorerRoot"));
        auto* tree = window.findChild<QTreeView*>(QStringLiteral("explorerTree"));
        root->click();
        QVERIFY(!tree->isVisibleTo(&window));
        auto* explorer = window.findChild<ketplus::ExplorerPanel*>();
        explorer->revealPath(explorer->rootPath() + QStringLiteral("/docs/guide.md"));
        QVERIFY(root->isChecked());
        QVERIFY(tree->isVisibleTo(&window));
    }

    void terminalSitsBelowTheEditorOnly() {
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace_.path());
        viewAction(window, QStringLiteral("Terminal"))->trigger();
        auto* terminal = window.findChild<ketplus::TerminalPanel*>();
        QVERIFY(terminal != nullptr);
        auto* column = window.findChild<QWidget*>(QStringLiteral("editorColumn"));
        QVERIFY(column->isAncestorOf(terminal));
        QVERIFY(
            !column->isAncestorOf(window.findChild<QTreeView*>(QStringLiteral("explorerTree"))));
    }

    void oneStatusBarAndTabIcons() {
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace_.path());
        window.openFile(workspace_.filePath(QStringLiteral("main.cpp")));
        window.openFile(workspace_.filePath(QStringLiteral("docs/guide.md")));

        // One status bar, as the Lite mock: nothing else in the editor column.
        QCOMPARE(window.findChild<QWidget*>(QStringLiteral("editorColumn"))
                     ->findChildren<ket::StatusBar*>()
                     .size(),
                 0);
        auto* row = window.statusBar()->findChild<ket::StatusBar*>();
        QVERIFY(row != nullptr);
        QStringList items;
        for (auto* item : row->findChildren<ket::StatusItem*>())
            items.append(item->text());
        QVERIFY(items.contains(QStringLiteral("Ln 1, Col 1")));
        QVERIFY(!items.contains(QStringLiteral("KétPlus")));
        QStringList buttons;
        for (auto* button : row->findChildren<QAbstractButton*>())
            if (button->isVisibleTo(row))
                buttons.append(button->accessibleName());
        QCOMPARE(buttons, (QStringList{QStringLiteral("Language"), QStringLiteral("File encoding"),
                                       QStringLiteral("Line endings")}));

        // Every item and menu button is centred in the bar, and the host is as tall as it.
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.statusBar()->height(), row->height());
        const int middle = (row->height() + 1) / 2; // below the 1 px top hairline
        for (auto* child : row->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
            if (!child->isVisibleTo(row))
                continue;
            QVERIFY2(qAbs(child->geometry().center().y() - middle) <= 1,
                     qPrintable(child->accessibleName()));
        }

        auto* tabs = window.findChild<QTabWidget*>();
        QVERIFY(tabs->count() >= 2);
        for (int index = 0; index < tabs->count(); ++index)
            QVERIFY2(!tabs->tabIcon(index).isNull(), qPrintable(tabs->tabText(index)));
        QVERIFY(tabs->cornerWidget(Qt::TopRightCorner) != nullptr);
    }

    void viewDiffFollowsTheFilesChanges() {
        QTemporaryDir repository;
        QVERIFY(repository.isValid());
        const auto git = [&repository](const QStringList& arguments) {
            QProcess process;
            process.setWorkingDirectory(repository.path());
            process.start(QStringLiteral("git"), arguments);
            return process.waitForFinished(10000) && process.exitCode() == 0;
        };
        write(repository.filePath(QStringLiteral("clean.txt")), "clean\n");
        write(repository.filePath(QStringLiteral("changed.txt")), "before\n");
        QVERIFY(git({QStringLiteral("init"), QStringLiteral("-q")}));
        QVERIFY(git({QStringLiteral("add"), QStringLiteral(".")}));
        QVERIFY(git({QStringLiteral("-c"), QStringLiteral("user.name=Test"), QStringLiteral("-c"),
                     QStringLiteral("user.email=test@example.com"), QStringLiteral("commit"),
                     QStringLiteral("-qm"), QStringLiteral("start")}));
        write(repository.filePath(QStringLiteral("changed.txt")), "after\n");

        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(repository.path());
        window.openFile(repository.filePath(QStringLiteral("changed.txt")));
        auto* tabs = window.findChild<QTabWidget*>();
        const auto viewDiff = [tabs] {
            return tabs->currentWidget()->findChild<QWidget*>(QStringLiteral("viewDiffButton"));
        };
        QTRY_VERIFY_WITH_TIMEOUT(viewDiff()->isVisibleTo(tabs), 10000);
        window.openFile(repository.filePath(QStringLiteral("clean.txt")));
        QVERIFY(viewDiff() != nullptr);
        QTRY_VERIFY(!viewDiff()->isVisibleTo(tabs));
    }

    void reviewCaptureRunsItsSteps() {
        QTemporaryDir out;
        QVERIFY(out.isValid());
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace_.path());
        window.show();
        const auto saved = out.filePath(QStringLiteral("terminal.png"));
        qputenv("KETPLUS_CAPTURE_STEPS", "View/Terminal");
        ketplus::runReviewCapture(theme, window, saved);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(saved), 5000);
        QVERIFY(window.findChild<ketplus::TerminalPanel*>() != nullptr);

        // A step that finds nothing fails the capture instead of saving the wrong state.
        const auto missing = out.filePath(QStringLiteral("missing.png"));
        qputenv("KETPLUS_CAPTURE_STEPS", "View/No Such Panel");
        ketplus::runReviewCapture(theme, window, missing);
        QTest::qWait(2000);
        QVERIFY(!QFile::exists(missing));
        qunsetenv("KETPLUS_CAPTURE_STEPS");
    }

  private:
    QTemporaryDir settings_;
    QTemporaryDir workspace_;
};

QTEST_MAIN(LiteWindowTest)
#include "LiteWindowTest.moc"
