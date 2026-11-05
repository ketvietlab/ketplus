#include "editor/DefinitionPattern.h"
#include "editor/EditorWidget.h"
#include "editor/SymbolExtractor.h"
#include "workspace/FuzzyMatcher.h"
#include "workspace/QuickOpenPopup.h"
#include "workspace/WorkspaceFileIndex.h"

#include <ScintillaMessages.h>

#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>

class NavigationTest final : public QObject {
    Q_OBJECT

  private slots:
    void scoresFuzzyMatches();
    void filtersAndActivatesQuickOpenItems();
    void collectsWorkspaceFiles();
    void extractsCppSymbols();
    void extractsScriptAndMarkdownSymbols();
    void movesCaretToPosition();
    void buildsDefinitionPatterns();
    void readsSymbolsAndFileTokensUnderTheCaret();
    void requestsDefinitionOnModifierClick();
};

void NavigationTest::buildsDefinitionPatterns() {
    const auto matches = [](const QString& pattern, const QString& line) {
        const QRegularExpression expression(pattern);
        return expression.isValid() && expression.match(line).hasMatch();
    };

    const QString cpp = ketplus::definitionExpression(QStringLiteral("Widget"),
                                                      QStringLiteral("c-cpp"));
    QVERIFY(matches(cpp, QStringLiteral("class Widget final : public QWidget {")));
    QVERIFY(!matches(cpp, QStringLiteral("    auto* widget = new Widget(this);")));

    const QString python = ketplus::definitionExpression(QStringLiteral("render"),
                                                         QStringLiteral("python"));
    QVERIFY(matches(python, QStringLiteral("def render(self):")));
    QVERIFY(!matches(python, QStringLiteral("    self.render()")));

    const QString js = ketplus::definitionExpression(QStringLiteral("handler"),
                                                     QStringLiteral("javascript-typescript"));
    QVERIFY(matches(js, QStringLiteral("const handler = (event) => {")));
    QVERIFY(matches(js, QStringLiteral("function handler(event) {")));

    const QString css = ketplus::definitionExpression(QStringLiteral("panel"),
                                                      QStringLiteral("css"));
    QVERIFY(matches(css, QStringLiteral(".panel { color: red; }")));

    // Not a name, so there is nothing to look for.
    QVERIFY(ketplus::definitionExpression(QStringLiteral("10"), QStringLiteral("c-cpp")).isEmpty());

    // An unrecognised syntax still finds the shapes other languages use.
    const QString unknown = ketplus::definitionExpression(QStringLiteral("helpUrl"),
                                                          QStringLiteral("generic"));
    QVERIFY(matches(unknown, QStringLiteral("export const helpUrl = 'https://example.com';")));
    QVERIFY(!matches(unknown, QStringLiteral("import { helpUrl } from '../data/help';")));

    // An import without an extension may mean any of several files.
    const QStringList candidates = ketplus::fileReferenceCandidates(QStringLiteral("../data/help"));
    QVERIFY(candidates.contains(QStringLiteral("../data/help")));
    QVERIFY(candidates.contains(QStringLiteral("../data/help.ts")));
    QVERIFY(candidates.contains(QStringLiteral("../data/help/index.ts")));
    QVERIFY(ketplus::fileReferenceCandidates(QStringLiteral("helpUrl")).isEmpty());
    // TypeScript projects import the built "./menus.js" from a file that is "./menus.ts".
    QVERIFY(ketplus::fileReferenceCandidates(QStringLiteral("./menus.js"))
                .contains(QStringLiteral("./menus.ts")));

    // The module a name is imported from, so a click follows the import.
    QCOMPARE(ketplus::importedModuleOnLine(
                 QStringLiteral("import { menus } from './menus.js';")),
             QStringLiteral("./menus.js"));
    QCOMPARE(ketplus::importedModuleOnLine(QStringLiteral("const a = require(\"../lib/a\")")),
             QStringLiteral("../lib/a"));
    QCOMPARE(ketplus::importedModuleOnLine(QStringLiteral("#include \"app/main.h\"")),
             QStringLiteral("app/main.h"));
    QCOMPARE(ketplus::importedModuleOnLine(QStringLiteral("from .helpers import render")),
             QStringLiteral("/helpers"));
    QVERIFY(ketplus::importedModuleOnLine(QStringLiteral("const menus = buildMenus();")).isEmpty());

    QVERIFY(ketplus::looksLikeFileReference(QStringLiteral("./theme.css")));
    QVERIFY(ketplus::looksLikeFileReference(QStringLiteral("app/main.h")));
    QVERIFY(!ketplus::looksLikeFileReference(QStringLiteral("renderWidget")));
}

void NavigationTest::readsSymbolsAndFileTokensUnderTheCaret() {
    ketplus::EditorWidget editor;
    const QByteArray text = "#include \"app/MainWindow.h\"\nWidget* widget = nullptr;\n";
    editor.setText(text);

    QCOMPARE(editor.wordAtPosition(text.indexOf("Widget*") + 2), QStringLiteral("Widget"));
    QCOMPARE(editor.fileTokenAtPosition(text.indexOf("app/MainWindow.h") + 4),
             QStringLiteral("app/MainWindow.h"));
    // A word is not a file reference, and whitespace has neither.
    QVERIFY(!ketplus::looksLikeFileReference(editor.fileTokenAtPosition(text.indexOf("* widget"))));
}

void NavigationTest::requestsDefinitionOnModifierClick() {
    ketplus::EditorWidget editor;
    editor.resize(420, 200);
    editor.setText("Widget* widget = nullptr;\n");
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    QSignalSpy spy(&editor, &ketplus::EditorWidget::definitionRequested);
    // Aim at the third character of the word, in the editor's own coordinates.
    const auto messageOf = [](const Scintilla::Message value) {
        return static_cast<unsigned int>(value);
    };
    const QPointF point(
        static_cast<qreal>(editor.send(messageOf(Scintilla::Message::PointXFromPosition), 0, 3)),
        static_cast<qreal>(editor.send(messageOf(Scintilla::Message::PointYFromPosition), 0, 3)) +
            2);
    QMouseEvent plain(QEvent::MouseButtonPress, point, editor.mapToGlobal(point), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(editor.viewport(), &plain);
    QCOMPARE(spy.count(), 0);

    QMouseEvent modified(QEvent::MouseButtonPress, point, editor.mapToGlobal(point),
                         Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    QApplication::sendEvent(editor.viewport(), &modified);
    QCOMPARE(spy.count(), 1);
}

void NavigationTest::scoresFuzzyMatches() {
    QCOMPARE(ketplus::fuzzyMatchScore(u"", u"anything"), 0);
    QCOMPARE(ketplus::fuzzyMatchScore(u"xyz", u"MainWindow.cpp"), -1);
    QVERIFY(ketplus::fuzzyMatchScore(u"mwc", u"src/app/MainWindow.cpp") >= 0);
    QVERIFY(ketplus::fuzzyMatchScore(u"mw", u"MainWindow") >
            ketplus::fuzzyMatchScore(u"mw", u"somewhere"));
    QVERIFY(ketplus::fuzzyMatchScore(u"main", u"src/main.cpp") >
            ketplus::fuzzyMatchScore(u"main", u"main/src/other.cpp"));
}

void NavigationTest::filtersAndActivatesQuickOpenItems() {
    QWidget window;
    window.resize(900, 600);
    ketplus::QuickOpenPopup popup(&window);
    window.show();

    popup.open(QStringLiteral("Go to file"),
               {{QStringLiteral("docs/readme.md"), {}, QStringLiteral("readme")},
                {QStringLiteral("src/app/MainWindow.cpp"), {}, QStringLiteral("main")},
                {QStringLiteral("src/editor/EditorWidget.cpp"), QStringLiteral("open"),
                 QStringLiteral("editor")}});
    QCOMPARE(popup.visibleItems().size(), 3);

    auto* queryEdit = popup.findChild<QLineEdit*>();
    QVERIFY(queryEdit != nullptr);
    queryEdit->setText(QStringLiteral("mwin"));
    QCOMPARE(popup.visibleItems().size(), 1);
    QCOMPARE(popup.visibleItems().first().label, QStringLiteral("src/app/MainWindow.cpp"));

    QSignalSpy activated(&popup, &ketplus::QuickOpenPopup::itemActivated);
    QTest::keyClick(queryEdit, Qt::Key_Return);
    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated.first().first().toString(), QStringLiteral("main"));
    QVERIFY(!popup.isVisible());
}

void NavigationTest::collectsWorkspaceFiles() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir root(directory.path());
    for (const QString& path :
         {QStringLiteral("a.txt"), QStringLiteral("src/b.cpp"), QStringLiteral("node_modules/x.js"),
          QStringLiteral(".git/config"), QStringLiteral("build/out.o")}) {
        QVERIFY(root.mkpath(QFileInfo(path).path()));
        QFile file(root.filePath(path));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    const auto files = ketplus::collectWorkspaceFiles(directory.path());
    QCOMPARE(files.relativePaths, QStringList({QStringLiteral("a.txt"), QStringLiteral("src/b.cpp")}));
    QVERIFY(!files.truncated);

    const auto limited = ketplus::collectWorkspaceFiles(directory.path(), 1);
    QCOMPARE(limited.relativePaths.size(), 1);
    QVERIFY(limited.truncated);

    const std::atomic_bool cancelled{true};
    const auto stopped = ketplus::collectWorkspaceFiles(directory.path(), 100, &cancelled);
    QVERIFY(stopped.relativePaths.isEmpty());
    QVERIFY(stopped.truncated);
}

void NavigationTest::extractsCppSymbols() {
    const QByteArray source = "namespace ketplus {\n"
                              "class MainWindow final : public QMainWindow {\n"
                              "enum class Mode { A };\n"
                              "void MainWindow::openFile(const QString& path) {\n"
                              "    if (path.isEmpty()) {\n"
                              "        helper(path);\n"
                              "    }\n"
                              "}\n"
                              "static int helper(int value)\n";
    const auto symbols = ketplus::extractDocumentSymbols(source, QStringLiteral("c-cpp"));

    QStringList names;
    for (const auto& symbol : symbols) {
        names.append(symbol.name);
    }
    QCOMPARE(names, QStringList({QStringLiteral("ketplus"), QStringLiteral("MainWindow"),
                                 QStringLiteral("Mode"), QStringLiteral("MainWindow::openFile"),
                                 QStringLiteral("helper")}));
    QCOMPARE(symbols.at(0).kind, QStringLiteral("namespace"));
    QCOMPARE(symbols.at(2).kind, QStringLiteral("enum"));
    QCOMPARE(symbols.at(3).kind, QStringLiteral("function"));
    QCOMPARE(symbols.at(3).line, 4);
}

void NavigationTest::extractsScriptAndMarkdownSymbols() {
    const auto python = ketplus::extractDocumentSymbols("class Foo:\n    def bar(self):\n        pass\n",
                                                        QStringLiteral("python"));
    QCOMPARE(python.size(), 2);
    QCOMPARE(python.at(1).name, QStringLiteral("bar"));
    QCOMPARE(python.at(1).kind, QStringLiteral("function"));
    QCOMPARE(python.at(1).depth, 1);

    const auto script = ketplus::extractDocumentSymbols(
        "export const load = async (id) => {\n}\nfunction save() {}\n",
        QStringLiteral("javascript-typescript"));
    QCOMPARE(script.size(), 2);
    QCOMPARE(script.at(0).name, QStringLiteral("load"));
    QCOMPARE(script.at(1).name, QStringLiteral("save"));

    const auto markdown =
        ketplus::extractDocumentSymbols("# Title\ntext\n## Details\n", QStringLiteral("markdown"));
    QCOMPARE(markdown.size(), 2);
    QCOMPARE(markdown.at(1).name, QStringLiteral("Details"));
    QCOMPARE(markdown.at(1).depth, 1);
    QCOMPARE(markdown.at(1).line, 3);

    QVERIFY(ketplus::extractDocumentSymbols("{\"a\": 1}", QStringLiteral("json")).isEmpty());
}

void NavigationTest::movesCaretToPosition() {
    ketplus::EditorWidget editor;
    editor.setText("one\ntwo\nthree");
    editor.setCaretPosition(5);
    QCOMPARE(editor.caretPosition(), 5);
    QCOMPARE(editor.currentLine(), 2);
    editor.setCaretPosition(999);
    QCOMPARE(editor.caretPosition(), 13);
}

QTEST_MAIN(NavigationTest)
#include "NavigationTest.moc"
