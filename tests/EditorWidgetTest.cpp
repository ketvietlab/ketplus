#include "editor/EditorWidget.h"
#include "ui/Theme.h"

#include <SciLexer.h>
#include <ScintillaMessages.h>
#include <QColor>
#include <QContextMenuEvent>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

namespace {

constexpr unsigned int message(const Scintilla::Message value) {
    return static_cast<unsigned int>(value);
}

int scintillaColor(const QString& value) {
    const QColor color(value);
    return color.red() | (color.green() << 8) | (color.blue() << 16);
}

} // namespace

class EditorWidgetTest final : public QObject {
    Q_OBJECT

  private slots:
    void editsThroughClipboardAndHistory();
    void findsForwardBackwardAndWraps();
    void replacesMatchesAsOneUndoStep();
    void matchesEditorTypography();
    void appliesCustomTypography();
    void persistsEditorSettings();
    void emitsContextMenuRequest();
    void selectsSyntaxFromPath_data();
    void selectsSyntaxFromPath();
    void highlightsGenericCode();
    void highlightsPopularLanguages_data();
    void highlightsPopularLanguages();
    void colorsYamlValuesOrange();
    void usesLargeFileModeAtThreshold();
    void hibernatesAndRestoresCleanFiles();
    void keepsDirtyFilesResident();
    void defersThemeChanges();
    void editsLinesAsUndoableSteps();
    void sortsAndTrimsLines();
    void togglesLineComments();
    void findsAndReplacesWithRegex();
    void limitsReplaceToSelection();
    void highlightsAllMatches();
    void navigatesLinesAndBookmarks();
    void jumpsToMatchingBrace();
    void autoClosesBracketsAndIndents();
    void appliesViewOptions();
    void persistsViewOptions();
    void sharesDocumentBetweenViews();
    void appliesZoom();
    void addsNextOccurrencesAsCursors();
    void addsCursorsVerticallyAndSplitsLines();
    void suggestsWordsFromDocument();
};

void EditorWidgetTest::editsThroughClipboardAndHistory() {
    ketplus::EditorWidget editor;
    editor.setText("KetPlus");

    editor.selectAllText();
    QVERIFY(editor.hasSelection());
    editor.cutSelection();
    QCOMPARE(editor.text(), QByteArray());

    editor.pasteClipboard();
    QCOMPARE(editor.text(), QByteArray("KetPlus"));
    QVERIFY(editor.canUndoEdit());
    editor.undoEdit();
    QCOMPARE(editor.text(), QByteArray());
    QVERIFY(editor.canRedoEdit());
    editor.redoEdit();
    QCOMPARE(editor.text(), QByteArray("KetPlus"));

    editor.selectAllText();
    editor.copySelection();
    editor.deleteSelection();
    QCOMPARE(editor.text(), QByteArray());
    editor.pasteClipboard();
    QCOMPARE(editor.text(), QByteArray("KetPlus"));
}

void EditorWidgetTest::findsForwardBackwardAndWraps() {
    ketplus::EditorWidget editor;
    editor.setText("one TWO one");

    auto result = editor.findText(QStringLiteral("one"), false, true, true);
    QVERIFY(result.found);
    QVERIFY(!result.wrapped);
    QCOMPARE(editor.selectedText(), QStringLiteral("one"));

    result = editor.findText(QStringLiteral("one"), false, true, true);
    QVERIFY(result.found);
    QVERIFY(!result.wrapped);

    result = editor.findText(QStringLiteral("one"), false, true, true);
    QVERIFY(result.found);
    QVERIFY(result.wrapped);

    result = editor.findText(QStringLiteral("two"), false, true, true);
    QVERIFY(!result.found);
    result = editor.findText(QStringLiteral("two"), false, false, true);
    QVERIFY(result.found);
    QCOMPARE(editor.selectedText(), QStringLiteral("TWO"));

    result = editor.findText(QStringLiteral("one"), true, true, true);
    QVERIFY(result.found);
}

void EditorWidgetTest::replacesMatchesAsOneUndoStep() {
    ketplus::EditorWidget editor;
    editor.setText("cat scatter cat CAT");

    QCOMPARE(editor.replaceAll(QStringLiteral("cat"), QStringLiteral("dog"), false, true), 3);
    QCOMPARE(editor.text(), QByteArray("dog scatter dog dog"));

    editor.undoEdit();
    QCOMPARE(editor.text(), QByteArray("cat scatter cat CAT"));

    QVERIFY(editor.findText(QStringLiteral("cat"), false, true, true).found);
    QVERIFY(editor.replaceSelection(QStringLiteral("cat"), QStringLiteral("fox"), true, true));
    QCOMPARE(editor.text(), QByteArray("cat scatter fox CAT"));
}

void EditorWidgetTest::matchesEditorTypography() {
    ketplus::EditorWidget editor;
    editor.applyTheme(ketplus::ThemePalette{});

    const int expectedSizeHundredthPoints = qRound(12.0 * 72.0 * 100.0 / editor.logicalDpiY());
    QCOMPARE(editor.send(message(Scintilla::Message::StyleGetSizeFractional), STYLE_DEFAULT),
             expectedSizeHundredthPoints);
    QCOMPARE(editor.send(message(Scintilla::Message::TextHeight), 0), 24);
}

void EditorWidgetTest::appliesCustomTypography() {
    auto settings = ketplus::EditorSettings::defaults();
    settings.fontSizePixels = 17;
    settings.lineHeightPixels = 31;

    ketplus::EditorWidget editor;
    editor.setEditorSettings(settings);
    editor.applyTheme(ketplus::ThemePalette{});

    const int expectedSizeHundredthPoints = qRound(17.0 * 72.0 * 100.0 / editor.logicalDpiY());
    QCOMPARE(editor.send(message(Scintilla::Message::StyleGetSizeFractional), STYLE_DEFAULT),
             expectedSizeHundredthPoints);
    QCOMPARE(editor.send(message(Scintilla::Message::TextHeight), 0), 31);
    QCOMPARE(editor.editorSettings(), settings);
}

void EditorWidgetTest::persistsEditorSettings() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString settingsPath = directory.filePath(QStringLiteral("settings.ini"));

    auto expected = ketplus::EditorSettings::defaults();
    expected.fontSizePixels = 16;
    expected.lineHeightPixels = 29;
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        expected.save(settings);
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
    }

    const QSettings settings(settingsPath, QSettings::IniFormat);
    QCOMPARE(ketplus::EditorSettings::load(settings), expected);
}

void EditorWidgetTest::emitsContextMenuRequest() {
    ketplus::EditorWidget editor;
    QSignalSpy requestSpy(&editor, &ketplus::EditorWidget::contextMenuRequested);
    const QPoint localPosition(32, 24);
    QContextMenuEvent event(QContextMenuEvent::Mouse, localPosition,
                            editor.mapToGlobal(localPosition));

    QVERIFY(QApplication::sendEvent(editor.viewport(), &event));
    QCOMPARE(requestSpy.count(), 1);
    QCOMPARE(requestSpy.first().first().toPoint(), localPosition);
    QVERIFY(event.isAccepted());
}

void EditorWidgetTest::selectsSyntaxFromPath_data() {
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("syntax");

    QTest::newRow("generic") << QStringLiteral("component.unknown")
                             << QStringLiteral("generic");
    QTest::newRow("plain text") << QStringLiteral("notes.txt") << QStringLiteral("plain");
    QTest::newRow("cpp") << QStringLiteral("main.cpp") << QStringLiteral("c-cpp");
    QTest::newRow("typescript") << QStringLiteral("app.tsx")
                                << QStringLiteral("javascript-typescript");
    QTest::newRow("python") << QStringLiteral("tool.py") << QStringLiteral("python");
    QTest::newRow("html") << QStringLiteral("index.html") << QStringLiteral("html");
    QTest::newRow("json") << QStringLiteral("package.json") << QStringLiteral("json");
    QTest::newRow("css") << QStringLiteral("theme.css") << QStringLiteral("css");
    QTest::newRow("shell") << QStringLiteral("install.zsh") << QStringLiteral("shell");
    QTest::newRow("yaml") << QStringLiteral("compose.yaml") << QStringLiteral("yaml");
    QTest::newRow("toml") << QStringLiteral("Cargo.toml") << QStringLiteral("toml");
    QTest::newRow("sql") << QStringLiteral("schema.sql") << QStringLiteral("sql");
    QTest::newRow("rust") << QStringLiteral("lib.rs") << QStringLiteral("rust");
    QTest::newRow("ruby") << QStringLiteral("server.rb") << QStringLiteral("ruby");
    QTest::newRow("lua") << QStringLiteral("init.lua") << QStringLiteral("lua");
    QTest::newRow("dart") << QStringLiteral("widget.dart") << QStringLiteral("dart");
    QTest::newRow("zig") << QStringLiteral("main.zig") << QStringLiteral("zig");
    QTest::newRow("cmake") << QStringLiteral("CMakeLists.txt") << QStringLiteral("cmake");
    QTest::newRow("make") << QStringLiteral("Makefile") << QStringLiteral("makefile");
    QTest::newRow("properties") << QStringLiteral(".env") << QStringLiteral("properties");
    QTest::newRow("diff") << QStringLiteral("change.patch") << QStringLiteral("diff");
}

void EditorWidgetTest::selectsSyntaxFromPath() {
    QFETCH(QString, path);
    QFETCH(QString, syntax);

    ketplus::EditorWidget editor;
    editor.configureLexerForPath(path);
    QCOMPARE(editor.syntaxName(), syntax);
}

void EditorWidgetTest::highlightsGenericCode() {
    const QByteArray code = "if (value == 42) return \"ok\"; // comment";
    ketplus::EditorWidget editor;
    editor.setText(code);
    editor.configureLexerForPath(QStringLiteral("source.unknown"));
    editor.applyTheme(ketplus::ThemePalette{});

    QCOMPARE(editor.syntaxName(), QStringLiteral("generic"));
    QCOMPARE(editor.send(message(Scintilla::Message::GetStyleAt), 0), SCE_C_WORD);
    QCOMPARE(editor.send(message(Scintilla::Message::GetStyleAt),
                         static_cast<uptr_t>(code.indexOf("42"))),
             SCE_C_NUMBER);
    QCOMPARE(editor.send(message(Scintilla::Message::GetStyleAt),
                         static_cast<uptr_t>(code.indexOf("\"ok\""))),
             SCE_C_STRING);
    QCOMPARE(editor.send(message(Scintilla::Message::GetStyleAt),
                         static_cast<uptr_t>(code.indexOf("//"))),
             SCE_C_COMMENTLINE);
}

void EditorWidgetTest::highlightsPopularLanguages_data() {
    QTest::addColumn<QString>("path");
    QTest::addColumn<QByteArray>("code");
    QTest::addColumn<int>("expectedStyle");

    QTest::newRow("javascript") << QStringLiteral("app.js") << QByteArray("const value = 1;")
                                << SCE_C_WORD;
    QTest::newRow("python") << QStringLiteral("app.py") << QByteArray("def greet():")
                            << SCE_P_WORD;
    QTest::newRow("shell") << QStringLiteral("run.sh") << QByteArray("if true; then")
                           << SCE_SH_WORD;
    QTest::newRow("sql") << QStringLiteral("query.sql") << QByteArray("select * from users")
                         << SCE_SQL_WORD;
    QTest::newRow("rust") << QStringLiteral("main.rs") << QByteArray("fn main() {}")
                          << SCE_RUST_WORD;
    QTest::newRow("ruby") << QStringLiteral("app.rb") << QByteArray("class Server")
                          << SCE_RB_WORD;
    QTest::newRow("lua") << QStringLiteral("init.lua") << QByteArray("local value = true")
                         << SCE_LUA_WORD;
    QTest::newRow("dart") << QStringLiteral("app.dart") << QByteArray("class Widget {}")
                          << SCE_DART_KW_PRIMARY;
    QTest::newRow("zig") << QStringLiteral("main.zig") << QByteArray("const value = 1;")
                         << SCE_ZIG_KW_PRIMARY;
}

void EditorWidgetTest::highlightsPopularLanguages() {
    QFETCH(QString, path);
    QFETCH(QByteArray, code);
    QFETCH(int, expectedStyle);

    ketplus::EditorWidget editor;
    editor.setText(code);
    editor.configureLexerForPath(path);
    editor.applyTheme(ketplus::ThemePalette{});

    QCOMPARE(editor.send(message(Scintilla::Message::GetStyleAt), 0), expectedStyle);
}

void EditorWidgetTest::colorsYamlValuesOrange() {
    const QByteArray code = "name: KetPlus\ncount: 42\nenabled: true\ndescription: |\n  Lightweight editor\n";
    ketplus::ThemePalette palette;
    palette.warning = QStringLiteral("#E5A93C");

    ketplus::EditorWidget editor;
    editor.setText(code);
    editor.configureLexerForPath(QStringLiteral("ketplus.yaml"));
    editor.applyTheme(palette);

    const int valueStyles[] = {SCE_YAML_DEFAULT, SCE_YAML_NUMBER, SCE_YAML_KEYWORD,
                               SCE_YAML_TEXT};
    const auto expectedColor = scintillaColor(palette.warning);
    for (const int style : valueStyles) {
        QCOMPARE(editor.send(message(Scintilla::Message::StyleGetFore),
                             static_cast<uptr_t>(style)),
                 expectedColor);
    }
}

void EditorWidgetTest::usesLargeFileModeAtThreshold() {
    ketplus::EditorWidget editor;
    const QByteArray content(ketplus::EditorWidget::largeFileThresholdBytes, 'x');

    editor.setText(content);
    editor.configureLexerForPath(QStringLiteral("large.cpp"));

    QVERIFY(editor.isLargeFileMode());
    QCOMPARE(editor.syntaxName(), QStringLiteral("large-file"));
    QCOMPARE(editor.residentBytes(), content.size());
    QVERIFY(!editor.canUndoEdit());
}

void EditorWidgetTest::hibernatesAndRestoresCleanFiles() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("sample.cpp"));
    const QByteArray content("first line\nsecond line\nthird line\n");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(content), content.size());
    file.close();

    ketplus::EditorWidget editor;
    const auto loadResult = editor.loadFile(path);
    QVERIFY2(loadResult.ok, qPrintable(loadResult.error));
    editor.send(message(Scintilla::Message::SetSel), 4, 15);

    QVERIFY(editor.hibernate());
    QVERIFY(editor.isHibernated());
    QCOMPARE(editor.residentBytes(), 0);
    QCOMPARE(editor.text(), QByteArray());

    const auto restoreResult = editor.restoreFromDisk();
    QVERIFY2(restoreResult.ok, qPrintable(restoreResult.error));
    QVERIFY(!editor.isHibernated());
    QCOMPARE(editor.text(), content);
    QCOMPARE(editor.send(message(Scintilla::Message::GetAnchor)), 4);
    QCOMPARE(editor.send(message(Scintilla::Message::GetCurrentPos)), 15);

    editor.send(message(Scintilla::Message::AddText), 1,
                reinterpret_cast<sptr_t>("x"));
    QVERIFY(editor.canUndoEdit());
}

void EditorWidgetTest::keepsDirtyFilesResident() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("dirty.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("clean"), 5);
    file.close();

    ketplus::EditorWidget editor;
    const auto loadResult = editor.loadFile(path);
    QVERIFY2(loadResult.ok, qPrintable(loadResult.error));
    editor.send(message(Scintilla::Message::AddText), 1,
                reinterpret_cast<sptr_t>("!"));

    QVERIFY(editor.document().isModified());
    QVERIFY(!editor.hibernate());
    QVERIFY(!editor.isHibernated());
    QCOMPARE(editor.text(), QByteArray("!clean"));
}

void EditorWidgetTest::defersThemeChanges() {
    ketplus::EditorWidget editor;
    editor.markThemePending();
    QVERIFY(editor.hasPendingTheme());

    editor.applyTheme(ketplus::ThemePalette{});
    QVERIFY(!editor.hasPendingTheme());
}

void EditorWidgetTest::editsLinesAsUndoableSteps() {
    ketplus::EditorWidget editor;
    editor.setText("alpha\nbeta\ngamma");

    editor.goToLine(2);
    editor.duplicateLines();
    QCOMPARE(editor.text(), QByteArray("alpha\nbeta\nbeta\ngamma"));
    editor.undoEdit();
    QCOMPARE(editor.text(), QByteArray("alpha\nbeta\ngamma"));

    editor.goToLine(2);
    editor.moveLinesUp();
    QCOMPARE(editor.text(), QByteArray("beta\nalpha\ngamma"));
    editor.undoEdit();

    editor.goToLine(3);
    editor.deleteLines();
    QCOMPARE(editor.text(), QByteArray("alpha\nbeta"));
    editor.undoEdit();
    QCOMPARE(editor.text(), QByteArray("alpha\nbeta\ngamma"));

    editor.setText("  one  \n   two\nthree");
    editor.goToLine(1);
    editor.joinLines();
    QCOMPARE(editor.text(), QByteArray("  one two\nthree"));

    editor.setText("one\r\ntwo");
    editor.goToLine(1);
    editor.duplicateLines();
    QCOMPARE(editor.text(), QByteArray("one\r\none\r\ntwo"));
}

void EditorWidgetTest::sortsAndTrimsLines() {
    ketplus::EditorWidget editor;
    editor.setText("pear\napple\npear\nfig\n");

    editor.sortLines(false);
    QCOMPARE(editor.text(), QByteArray("apple\nfig\npear\npear\n"));
    editor.sortLines(true);
    QCOMPARE(editor.text(), QByteArray("apple\nfig\npear\n"));

    editor.setText("a  \nb\t\nc");
    editor.trimTrailingWhitespace();
    QCOMPARE(editor.text(), QByteArray("a\nb\nc"));
    editor.undoEdit();
    QCOMPARE(editor.text(), QByteArray("a  \nb\t\nc"));
}

void EditorWidgetTest::togglesLineComments() {
    ketplus::EditorWidget editor;
    editor.setText("int a;\n  int b;\n");
    editor.configureLexerForPath(QStringLiteral("sample.cpp"));
    editor.send(message(Scintilla::Message::SetSel), 0, 15);

    QVERIFY(editor.toggleComment());
    QCOMPARE(editor.text(), QByteArray("// int a;\n//   int b;\n"));
    QVERIFY(editor.toggleComment());
    QCOMPARE(editor.text(), QByteArray("int a;\n  int b;\n"));

    editor.setText("x = 1");
    editor.configureLexerForPath(QStringLiteral("script.py"));
    QVERIFY(editor.toggleComment());
    QCOMPARE(editor.text(), QByteArray("# x = 1"));

    editor.setText("<p>");
    editor.configureLexerForPath(QStringLiteral("page.html"));
    QVERIFY(editor.toggleComment());
    QCOMPARE(editor.text(), QByteArray("<!-- <p> -->"));
    QVERIFY(editor.toggleComment());
    QCOMPARE(editor.text(), QByteArray("<p>"));

    editor.setText("{}");
    editor.configureLexerForPath(QStringLiteral("data.json"));
    QVERIFY(!editor.toggleComment());
}

void EditorWidgetTest::findsAndReplacesWithRegex() {
    ketplus::EditorWidget editor;
    editor.setText("id=12 name=x id=345");
    const ketplus::SearchOptions options{.regex = true};

    const auto result = editor.findText(QStringLiteral("id=([0-9]+)"), false, options);
    QVERIFY(result.found);
    QCOMPARE(editor.selectedText(), QStringLiteral("id=12"));

    QCOMPARE(editor.replaceAll(QStringLiteral("id=([0-9]+)"), QStringLiteral("#\\1"), options), 2);
    QCOMPARE(editor.text(), QByteArray("#12 name=x #345"));

    editor.setText("a\nb");
    QCOMPARE(editor.replaceAll(QStringLiteral("^"), QStringLiteral("> "), options), 2);
    QCOMPARE(editor.text(), QByteArray("> a\n> b"));
}

void EditorWidgetTest::limitsReplaceToSelection() {
    ketplus::EditorWidget editor;
    editor.setText("cat cat cat");
    editor.send(message(Scintilla::Message::SetSel), 4, 11);
    editor.setSearchScopeToSelection();
    QVERIFY(editor.hasSearchScope());
    const ketplus::SearchOptions options{.matchCase = true, .inSelection = true};

    QCOMPARE(editor.replaceAll(QStringLiteral("cat"), QStringLiteral("tiger"), options), 2);
    QCOMPARE(editor.text(), QByteArray("cat tiger tiger"));
    QCOMPARE(editor.replaceAll(QStringLiteral("tiger"), QStringLiteral("ox"), options), 2);
    QCOMPARE(editor.text(), QByteArray("cat ox ox"));
    QCOMPARE(editor.replaceAll(QStringLiteral("cat"), QStringLiteral("dog"), options), 0);

    editor.clearSearchScope();
    QCOMPARE(editor.replaceAll(QStringLiteral("cat"), QStringLiteral("dog"), options), 1);
}

void EditorWidgetTest::highlightsAllMatches() {
    constexpr uptr_t findIndicator = 8;
    ketplus::EditorWidget editor;
    editor.setText("one two one two one");

    QCOMPARE(editor.highlightMatches(QStringLiteral("one"), ketplus::SearchOptions{.matchCase = true}),
             3);
    QCOMPARE(editor.send(message(Scintilla::Message::IndicatorValueAt), findIndicator, 0), 1);
    QCOMPARE(editor.send(message(Scintilla::Message::IndicatorValueAt), findIndicator, 4), 0);

    editor.clearMatchHighlights();
    QCOMPARE(editor.send(message(Scintilla::Message::IndicatorValueAt), findIndicator, 0), 0);
    QCOMPARE(editor.highlightMatches(QString(), ketplus::SearchOptions{}), 0);
}

void EditorWidgetTest::navigatesLinesAndBookmarks() {
    ketplus::EditorWidget editor;
    editor.setText("a\nb\nc\nd\n");
    QCOMPARE(editor.lineCount(), 5);

    editor.goToLine(3);
    QCOMPARE(editor.currentLine(), 3);
    editor.toggleBookmark();
    QVERIFY(editor.hasBookmark(3));
    editor.goToLine(1);
    editor.toggleBookmark();

    QVERIFY(editor.goToNextBookmark());
    QCOMPARE(editor.currentLine(), 3);
    QVERIFY(editor.goToNextBookmark());
    QCOMPARE(editor.currentLine(), 1);
    QVERIFY(editor.goToPreviousBookmark());
    QCOMPARE(editor.currentLine(), 3);

    editor.clearBookmarks();
    QVERIFY(!editor.hasBookmark(3));
    QVERIFY(!editor.goToNextBookmark());

    editor.goToLine(99);
    QCOMPARE(editor.currentLine(), 5);
}

void EditorWidgetTest::jumpsToMatchingBrace() {
    ketplus::EditorWidget editor;
    editor.setText("f(a[1]) x");

    editor.send(message(Scintilla::Message::SetEmptySelection), 1);
    QVERIFY(editor.jumpToMatchingBrace());
    QCOMPARE(editor.send(message(Scintilla::Message::GetCurrentPos)), 6);
    QVERIFY(editor.jumpToMatchingBrace());
    QCOMPARE(editor.send(message(Scintilla::Message::GetCurrentPos)), 1);

    editor.send(message(Scintilla::Message::SetEmptySelection), 9);
    QVERIFY(!editor.jumpToMatchingBrace());
}

void EditorWidgetTest::autoClosesBracketsAndIndents() {
    ketplus::EditorWidget editor;
    editor.setText("");

    QTest::keyClick(&editor, '(');
    QCOMPARE(editor.text(), QByteArray("()"));
    QTest::keyClick(&editor, ')');
    QCOMPARE(editor.text(), QByteArray("()"));
    QCOMPARE(editor.send(message(Scintilla::Message::GetCurrentPos)), 2);

    editor.setText("if (x) {");
    editor.send(message(Scintilla::Message::SetEmptySelection), 8);
    QTest::keyClick(&editor, Qt::Key_Return);
    QCOMPARE(editor.text(), QByteArray("if (x) {\n    "));

    auto options = editor.viewOptions();
    options.autoCloseBrackets = false;
    editor.setViewOptions(options);
    editor.setText("");
    QTest::keyClick(&editor, '[');
    QCOMPARE(editor.text(), QByteArray("["));
}

void EditorWidgetTest::appliesViewOptions() {
    ketplus::EditorWidget editor;
    editor.setText("int main() {\n  return 0;\n}\n");
    editor.configureLexerForPath(QStringLiteral("main.cpp"));

    ketplus::EditorViewOptions options;
    options.wordWrap = true;
    options.showWhitespace = true;
    options.showRuler = true;
    options.rulerColumn = 80;
    options.tabWidth = 2;
    options.useTabs = true;
    editor.setViewOptions(options);

    QCOMPARE(editor.send(message(Scintilla::Message::GetWrapMode)), 1);
    QCOMPARE(editor.send(message(Scintilla::Message::GetViewWS)), 1);
    QCOMPARE(editor.send(message(Scintilla::Message::GetEdgeMode)), 1);
    QCOMPARE(editor.send(message(Scintilla::Message::GetEdgeColumn)), 80);
    QCOMPARE(editor.send(message(Scintilla::Message::GetTabWidth)), 2);
    QCOMPARE(editor.send(message(Scintilla::Message::GetUseTabs)), 1);
    QVERIFY(editor.send(message(Scintilla::Message::GetMarginWidthN), 2) > 0);

    options.codeFolding = false;
    editor.setViewOptions(options);
    QCOMPARE(editor.send(message(Scintilla::Message::GetMarginWidthN), 2), 0);

    ketplus::EditorWidget largeEditor;
    options.codeFolding = true;
    largeEditor.setViewOptions(options);
    largeEditor.setText(QByteArray(ketplus::EditorWidget::largeFileThresholdBytes, 'x'));
    largeEditor.configureLexerForPath(QStringLiteral("large.cpp"));
    QCOMPARE(largeEditor.send(message(Scintilla::Message::GetWrapMode)), 0);
    QCOMPARE(largeEditor.send(message(Scintilla::Message::GetMarginWidthN), 2), 0);
}

void EditorWidgetTest::persistsViewOptions() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("view.ini")), QSettings::IniFormat);

    ketplus::EditorViewOptions options;
    options.wordWrap = true;
    options.tabWidth = 99;
    options.rulerColumn = 0;
    options.save(settings);

    const auto loaded = ketplus::EditorViewOptions::load(settings);
    QVERIFY(loaded.wordWrap);
    QCOMPARE(loaded.tabWidth, ketplus::EditorViewOptions::maximumTabWidth);
    QCOMPARE(loaded.rulerColumn, ketplus::EditorViewOptions::minimumRulerColumn);

    options.zoom = 99;
    options.save(settings);
    QCOMPARE(ketplus::EditorViewOptions::load(settings).zoom,
             ketplus::EditorViewOptions::maximumZoom);
}

void EditorWidgetTest::sharesDocumentBetweenViews() {
    ketplus::EditorWidget source;
    source.setText("hello");
    source.configureLexerForPath(QStringLiteral("sample.cpp"));

    {
        ketplus::EditorWidget mirror;
        mirror.shareDocumentWith(source);
        QCOMPARE(mirror.text(), QByteArray("hello"));
        QCOMPARE(mirror.syntaxName(), source.syntaxName());

        mirror.send(message(Scintilla::Message::AppendText), 1, reinterpret_cast<sptr_t>("!"));
        QCOMPARE(source.text(), QByteArray("hello!"));
        QVERIFY(source.document().isModified());
    }

    source.send(message(Scintilla::Message::AppendText), 1, reinterpret_cast<sptr_t>("?"));
    QCOMPARE(source.text(), QByteArray("hello!?"));
}

void EditorWidgetTest::appliesZoom() {
    ketplus::EditorWidget editor;
    auto options = editor.viewOptions();
    options.zoom = 3;
    editor.setViewOptions(options);
    QCOMPARE(editor.send(message(Scintilla::Message::GetZoom)), 3);

    options.zoom = -50;
    editor.setViewOptions(options);
    QCOMPARE(editor.send(message(Scintilla::Message::GetZoom)),
             ketplus::EditorViewOptions::minimumZoom);
}

void EditorWidgetTest::addsNextOccurrencesAsCursors() {
    ketplus::EditorWidget editor;
    editor.setText("foo bar foo baz foo");
    editor.send(message(Scintilla::Message::SetEmptySelection), 1);

    editor.addNextOccurrence();
    QCOMPARE(editor.selectedText(), QStringLiteral("foo"));
    QCOMPARE(editor.selectionCount(), 1);
    editor.addNextOccurrence();
    QCOMPARE(editor.selectionCount(), 2);
    editor.selectAllOccurrences();
    QCOMPARE(editor.selectionCount(), 3);

    QTest::keyClicks(&editor, QStringLiteral("x"));
    QCOMPARE(editor.text(), QByteArray("x bar x baz x"));
    editor.collapseToMainSelection();
    QCOMPARE(editor.selectionCount(), 1);
}

void EditorWidgetTest::addsCursorsVerticallyAndSplitsLines() {
    ketplus::EditorWidget editor;
    editor.setText("abc\nabc\nabc");
    editor.send(message(Scintilla::Message::SetEmptySelection), 1);

    editor.addCursorVertically(false);
    editor.addCursorVertically(false);
    QCOMPARE(editor.selectionCount(), 3);
    editor.addCursorVertically(false);
    QCOMPARE(editor.selectionCount(), 3);
    QTest::keyClicks(&editor, QStringLiteral("-"));
    QCOMPARE(editor.text(), QByteArray("a-bc\na-bc\na-bc"));

    editor.setText("one\ntwo\nthree");
    editor.send(message(Scintilla::Message::SetSel), 0, 13);
    editor.splitSelectionIntoLines();
    QCOMPARE(editor.selectionCount(), 3);
}

void EditorWidgetTest::suggestsWordsFromDocument() {
    ketplus::EditorWidget editor;
    editor.setText("configure configuration 123abc\n");
    editor.send(message(Scintilla::Message::SetEmptySelection),
                static_cast<uptr_t>(editor.send(message(Scintilla::Message::GetLength))));

    QTest::keyClicks(&editor, QStringLiteral("con"));
    QVERIFY(editor.send(message(Scintilla::Message::AutoCActive)) != 0);
    editor.send(message(Scintilla::Message::AutoCCancel));

    auto options = editor.viewOptions();
    options.wordCompletion = false;
    editor.setViewOptions(options);
    QTest::keyClicks(&editor, QStringLiteral("f"));
    QVERIFY(editor.send(message(Scintilla::Message::AutoCActive)) == 0);

    QVERIFY(editor.showWordCompletions(true));
    editor.send(message(Scintilla::Message::AutoCCancel));
    QTest::keyClicks(&editor, QStringLiteral("zzz"));
    QVERIFY(!editor.showWordCompletions(true));
}

QTEST_MAIN(EditorWidgetTest)
#include "EditorWidgetTest.moc"
