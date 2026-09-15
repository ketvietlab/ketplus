#include "editor/EditorWidget.h"
#include "editor/SyntaxDefinition.h"
#include "workspace/SearchPanel.h"
#include "workspace/WorkspaceSearch.h"

#include <ScintillaMessages.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>

namespace {

constexpr unsigned int message(const Scintilla::Message value) {
    return static_cast<unsigned int>(value);
}

bool writeFile(const QString& path, const QByteArray& content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

} // namespace

class SearchViewTest final : public QObject {
    Q_OBJECT

  private slots:
    void buildsSearchExpressions();
    void searchesTextLineByLine();
    void replacesTextLineByLine();
    void filtersIncludePatterns();
    void readsOnlySearchableFiles();
    void searchesWorkspaceWithOpenBuffers();
    void collectsSearchPanelResults();
    void highlightsSelectedWordMatches();
    void convertsLineEndingsAndOverridesSyntax();
    void replacesAllTextAsOneUndoStep();
    void listsSyntaxChoices();
};

void SearchViewTest::buildsSearchExpressions() {
    QString error;
    auto expression = ketplus::buildSearchExpression({.query = QStringLiteral("a.b")}, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(expression.match(QStringLiteral("xa.by")).hasMatch());
    QVERIFY(!expression.match(QStringLiteral("axb")).hasMatch());

    expression = ketplus::buildSearchExpression(
        {.query = QStringLiteral("cat"), .matchCase = true, .wholeWord = true}, &error);
    QVERIFY(expression.match(QStringLiteral("a cat")).hasMatch());
    QVERIFY(!expression.match(QStringLiteral("a Cat")).hasMatch());
    QVERIFY(!expression.match(QStringLiteral("scatter")).hasMatch());

    expression = ketplus::buildSearchExpression({.query = QStringLiteral("("), .regex = true}, &error);
    QVERIFY(!error.isEmpty());
    ketplus::buildSearchExpression({}, &error);
    QVERIFY(!error.isEmpty());
}

void SearchViewTest::searchesTextLineByLine() {
    QString error;
    const auto expression = ketplus::buildSearchExpression({.query = QStringLiteral("one")}, &error);
    const auto matches =
        ketplus::searchText(QStringLiteral("one two\r\nthree One\n"), expression, 100);

    QCOMPARE(matches.size(), 2);
    QCOMPARE(matches.at(0).line, 1);
    QCOMPARE(matches.at(0).column, 0);
    QCOMPARE(matches.at(0).length, 3);
    QCOMPARE(matches.at(0).preview, QStringLiteral("one two"));
    QCOMPARE(matches.at(1).line, 2);
    QCOMPARE(matches.at(1).column, 6);
    QCOMPARE(ketplus::searchText(QStringLiteral("one one one"), expression, 2).size(), 2);
}

void SearchViewTest::replacesTextLineByLine() {
    QString error;
    int replacements = 0;
    const auto plain = ketplus::buildSearchExpression({.query = QStringLiteral("a.b")}, &error);
    QCOMPARE(ketplus::replaceInText(QStringLiteral("a.b\r\naxb a.b"), plain, QStringLiteral("$1"),
                                    false, &replacements),
             QStringLiteral("$1\r\naxb $1"));
    QCOMPARE(replacements, 2);

    const auto regex = ketplus::buildSearchExpression(
        {.query = QStringLiteral("id=(\\d+)"), .regex = true}, &error);
    QCOMPARE(ketplus::replaceInText(QStringLiteral("id=12 id=3\nnone"), regex,
                                    QStringLiteral("#\\1 $1"), true, &replacements),
             QStringLiteral("#12 12 #3 3\nnone"));
    QCOMPARE(replacements, 2);

    const auto lineStart =
        ketplus::buildSearchExpression({.query = QStringLiteral("^"), .regex = true}, &error);
    QCOMPARE(ketplus::replaceInText(QStringLiteral("a\nb"), lineStart, QStringLiteral("> "), true,
                                    &replacements),
             QStringLiteral("> a\n> b"));
}

void SearchViewTest::filtersIncludePatterns() {
    QVERIFY(ketplus::matchesIncludePatterns(QStringLiteral("src/a.cpp"), QString()));
    QVERIFY(ketplus::matchesIncludePatterns(QStringLiteral("src/a.cpp"), QStringLiteral("*.cpp")));
    QVERIFY(!ketplus::matchesIncludePatterns(QStringLiteral("src/a.h"), QStringLiteral("*.cpp")));
    QVERIFY(ketplus::matchesIncludePatterns(QStringLiteral("src/x/y.h"), QStringLiteral("src/**")));
    QVERIFY(ketplus::matchesIncludePatterns(QStringLiteral("src/x/y.h"), QStringLiteral("src/")));
    QVERIFY(!ketplus::matchesIncludePatterns(QStringLiteral("lib/src.h"), QStringLiteral("src/**")));
    QVERIFY(ketplus::matchesIncludePatterns(QStringLiteral("docs/a.md"),
                                            QStringLiteral("*.cpp, *.md")));
}

void SearchViewTest::readsOnlySearchableFiles() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString text = directory.filePath(QStringLiteral("text.txt"));
    const QString binary = directory.filePath(QStringLiteral("binary.bin"));
    const QString latin1 = directory.filePath(QStringLiteral("latin1.txt"));
    QVERIFY(writeFile(text, QByteArray("\xEF\xBB\xBFXin ch\xC3\xA0o")));
    QVERIFY(writeFile(binary, QByteArray("abc\0def", 7)));
    QVERIFY(writeFile(latin1, QByteArray("caf\xE9")));

    QString content;
    QVERIFY(ketplus::readSearchableFile(text, &content));
    QCOMPARE(content, QStringLiteral("Xin chào"));
    QVERIFY(!ketplus::readSearchableFile(binary, &content));
    QVERIFY(!ketplus::readSearchableFile(latin1, &content));
    QVERIFY(!ketplus::readSearchableFile(directory.filePath(QStringLiteral("missing")), &content));
}

void SearchViewTest::searchesWorkspaceWithOpenBuffers() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir root(directory.path());
    QVERIFY(root.mkpath(QStringLiteral("src")));
    QVERIFY(writeFile(root.filePath(QStringLiteral("a.txt")), "hello\n"));
    QVERIFY(writeFile(root.filePath(QStringLiteral("src/b.cpp")), "hello world\n"));
    QVERIFY(writeFile(root.filePath(QStringLiteral("data.bin")), QByteArray("hello\0", 6)));

    QString error;
    const ketplus::WorkspaceSearchOptions options{.query = QStringLiteral("hello")};
    const auto expression = ketplus::buildSearchExpression(options, &error);
    const QHash<QString, QString> buffers{
        {QDir::cleanPath(root.absoluteFilePath(QStringLiteral("src/b.cpp"))), QStringLiteral("bye")}};

    QStringList found;
    const auto summary = ketplus::searchWorkspace(
        root.absolutePath(), options, expression, buffers, nullptr,
        [&found](const ketplus::WorkspaceSearchFileResult& file) { found.append(file.relativePath); });
    QCOMPARE(found, QStringList{QStringLiteral("a.txt")});
    QCOMPARE(summary.fileCount, 1);
    QCOMPARE(summary.matchCount, 1);

    auto filtered = options;
    filtered.includePatterns = QStringLiteral("*.cpp");
    QCOMPARE(ketplus::searchWorkspace(root.absolutePath(), filtered, expression, {}, nullptr, {})
                 .fileCount,
             1);

    const std::atomic_bool cancelled{true};
    QCOMPARE(ketplus::searchWorkspace(root.absolutePath(), options, expression, {}, &cancelled, {})
                 .matchCount,
             0);
}

void SearchViewTest::collectsSearchPanelResults() {
    ketplus::SearchPanel panel;
    panel.addFileResult({QStringLiteral("/tmp/a.txt"), QStringLiteral("a.txt"),
                         {{1, 0, 5, QStringLiteral("hello")}, {3, 2, 5, QStringLiteral("  hello")}}});
    panel.addFileResult(
        {QStringLiteral("/tmp/b.txt"), QStringLiteral("b.txt"), {{2, 0, 5, QStringLiteral("hello")}}});

    QCOMPARE(panel.resultPaths(),
             QStringList({QStringLiteral("/tmp/a.txt"), QStringLiteral("/tmp/b.txt")}));
    QCOMPARE(panel.resultMatchCount(), 3);
    panel.clearResults();
    QVERIFY(panel.resultPaths().isEmpty());
    QCOMPARE(panel.resultMatchCount(), 0);
}

void SearchViewTest::highlightsSelectedWordMatches() {
    constexpr uptr_t selectionIndicator = 9;
    ketplus::EditorWidget editor;
    editor.setText("foo bar foo foobar");

    editor.send(message(Scintilla::Message::SetSel), 0, 3);
    QCOMPARE(editor.highlightSelectionMatches(), 1);
    QCOMPARE(editor.send(message(Scintilla::Message::IndicatorValueAt), selectionIndicator, 8), 1);
    QCOMPARE(editor.send(message(Scintilla::Message::IndicatorValueAt), selectionIndicator, 12), 0);

    editor.send(message(Scintilla::Message::SetSel), 0, 2);
    QCOMPARE(editor.highlightSelectionMatches(), 0);
    QCOMPARE(editor.send(message(Scintilla::Message::IndicatorValueAt), selectionIndicator, 8), 0);

    auto options = editor.viewOptions();
    options.highlightSelectionMatches = false;
    editor.setViewOptions(options);
    editor.send(message(Scintilla::Message::SetSel), 0, 3);
    QCOMPARE(editor.highlightSelectionMatches(), 0);
}

void SearchViewTest::convertsLineEndingsAndOverridesSyntax() {
    ketplus::EditorWidget editor;
    editor.setText("a\nb");
    QCOMPARE(editor.lineEnding(), ketplus::LineEnding::Lf);
    editor.setLineEnding(ketplus::LineEnding::CrLf);
    QCOMPARE(editor.text(), QByteArray("a\r\nb"));
    QCOMPARE(editor.lineEnding(), ketplus::LineEnding::CrLf);
    editor.undoEdit();
    QCOMPARE(editor.text(), QByteArray("a\nb"));

    editor.configureLexerForPath(QStringLiteral("notes.txt"));
    QCOMPARE(editor.syntaxName(), QStringLiteral("plain"));
    editor.setSyntaxOverride(QStringLiteral("python"));
    QCOMPARE(editor.syntaxName(), QStringLiteral("python"));
    QCOMPARE(editor.syntaxOverride(), QStringLiteral("python"));
    editor.configureLexerForPath(QStringLiteral("renamed.txt"));
    QCOMPARE(editor.syntaxName(), QStringLiteral("python"));

    editor.setSyntaxOverride(QStringLiteral("not-a-language"));
    QVERIFY(editor.syntaxOverride().isEmpty());
    editor.configureLexerForPath(QStringLiteral("notes.txt"));
    QCOMPARE(editor.syntaxName(), QStringLiteral("plain"));
}

void SearchViewTest::replacesAllTextAsOneUndoStep() {
    ketplus::EditorWidget editor;
    editor.setText("x\n\xC3\xA1 y");

    editor.replaceAllText("changed");
    QCOMPARE(editor.text(), QByteArray("changed"));
    QVERIFY(editor.document().isModified());
    editor.undoEdit();
    QCOMPARE(editor.text(), QByteArray("x\n\xC3\xA1 y"));

    editor.selectTextRange(2, 2, 1);
    QCOMPARE(editor.selectedText(), QStringLiteral("y"));
}

void SearchViewTest::listsSyntaxChoices() {
    const auto& choices = ketplus::syntaxChoices();
    QVERIFY(choices.size() >= 28);
    for (std::size_t index = 1; index < choices.size(); ++index) {
        QVERIFY(QLatin1String(choices[index - 1].displayName)
                    .compare(QLatin1String(choices[index].displayName), Qt::CaseInsensitive) <=
                0);
    }
    QVERIFY(ketplus::syntaxDefinitionByName(QStringLiteral("rust")) != nullptr);
    QVERIFY(ketplus::syntaxDefinitionByName(QStringLiteral("nope")) == nullptr);
    QCOMPARE(ketplus::syntaxDisplayName(QStringLiteral("c-cpp")), QStringLiteral("C / C++"));
}

QTEST_MAIN(SearchViewTest)
#include "SearchViewTest.moc"
