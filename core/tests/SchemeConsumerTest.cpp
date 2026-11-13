#include "editor/EditorWidget.h"
#include "editor/SyntaxRoleAdapter.h"
#include "terminal/TerminalSession.h"
#include "terminal/TerminalView.h"
#include "ui/theme/ColorScheme.h"
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <SciLexer.h>
#include <Scintilla.h>
#include <ScintillaMessages.h>
using namespace ketplus;
namespace {
constexpr unsigned int message(Scintilla::Message value) {
    return static_cast<unsigned int>(value);
}
int rgb(const QColor& color) { return color.red() | (color.green() << 8) | (color.blue() << 16); }
} // namespace
class SchemeConsumerTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName(QStringLiteral("KetPlusTests"));
        QCoreApplication::setApplicationName(QStringLiteral("SchemeConsumers"));
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QVERIFY(settings_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    }
    void roleMapping_data() {
        QTest::addColumn<QByteArray>("lexer");
        QTest::addColumn<int>("style");
        QTest::addColumn<QString>("role");
        QTest::newRow("cpp-keyword")
            << QByteArray("cpp") << SCE_C_WORD << QStringLiteral("keyword");
        QTest::newRow("python-comment")
            << QByteArray("python") << SCE_P_COMMENTLINE << QStringLiteral("comment");
        QTest::newRow("json-string")
            << QByteArray("json") << SCE_JSON_STRING << QStringLiteral("string");
    }
    void roleMapping() {
        QFETCH(QByteArray, lexer);
        QFETCH(int, style);
        QFETCH(QString, role);
        auto scheme = builtinColorScheme(SchemeKind::Light);
        scheme.ui["chrome.accent"] = QColor(255, 0, 0);
        scheme.syntax[role] = {QColor(1, 2, 3), true, true, true};
        const auto styles = lexillaSchemeStyles(lexer.toStdString(), scheme);
        QCOMPARE(styles[static_cast<std::size_t>(style)].fg, QColor(1, 2, 3));
        QVERIFY(styles[static_cast<std::size_t>(style)].bold);
        QVERIFY(styles[static_cast<std::size_t>(style)].italic);
        QVERIFY(styles[static_cast<std::size_t>(style)].underline);
    }
    void builtinEditorStateParity() {
        EditorWidget editor;
        editor.configureLexerForPath(QStringLiteral("file.cpp"));
        const auto state = [&] {
            return QList<sptr_t>{
                editor.send(message(Scintilla::Message::StyleGetFore), STYLE_DEFAULT),
                editor.send(message(Scintilla::Message::StyleGetBack), STYLE_DEFAULT),
                editor.send(message(Scintilla::Message::StyleGetFore), STYLE_LINENUMBER),
                editor.send(message(Scintilla::Message::StyleGetBack), STYLE_LINENUMBER),
                editor.send(message(Scintilla::Message::StyleGetFore), SCE_C_WORD),
                editor.send(message(Scintilla::Message::GetCaretFore)),
                editor.send(message(Scintilla::Message::GetCaretLineBack)),
                editor.send(message(Scintilla::Message::GetCaretLineBackAlpha))};
        };
        for (auto kind : {SchemeKind::Light, SchemeKind::Dark}) {
            const auto scheme = builtinColorScheme(kind);
            editor.applyTheme(colorSchemePalette(scheme));
            const auto legacy = state();
            editor.applyColorScheme(scheme);
            QCOMPARE(state(), legacy);
        }
    }
    void editorPreservesDocumentAndUndo() {
        EditorWidget editor;
        editor.resize(500, 250);
        QByteArray content;
        for (int i = 0; i < 60; ++i)
            content += "int value = 1; // line\n";
        editor.replaceAllText(content);
        editor.configureLexerForPath(QStringLiteral("file.cpp"));
        editor.send(message(Scintilla::Message::SetSel), 5, 17);
        editor.send(message(Scintilla::Message::SetFirstVisibleLine), 10);
        const auto* document = &editor.document();
        const auto firstVisible = editor.send(message(Scintilla::Message::GetFirstVisibleLine));
        const auto canUndo = editor.send(message(Scintilla::Message::CanUndo));
        auto scheme = builtinColorScheme(SchemeKind::Dark);
        scheme.syntax["keyword"] = {QColor(11, 22, 33), true, true, true};
        scheme.editor["bg"] = QColor(1, 2, 3);
        scheme.editor["caret"] = QColor(44, 55, 66);
        editor.applyColorScheme(scheme);
        QCOMPARE(&editor.document(), document);
        QCOMPARE(editor.text(), content);
        QCOMPARE(editor.send(message(Scintilla::Message::GetSelectionStart)), sptr_t(5));
        QCOMPARE(editor.send(message(Scintilla::Message::GetSelectionEnd)), sptr_t(17));
        QCOMPARE(editor.send(message(Scintilla::Message::GetFirstVisibleLine)), firstVisible);
        QCOMPARE(editor.send(message(Scintilla::Message::CanUndo)), canUndo);
        QCOMPARE(editor.send(message(Scintilla::Message::StyleGetFore), SCE_C_WORD),
                 sptr_t(rgb(QColor(11, 22, 33))));
        QCOMPARE(editor.send(message(Scintilla::Message::StyleGetBold), SCE_C_WORD), sptr_t(1));
        QCOMPARE(editor.send(message(Scintilla::Message::StyleGetItalic), SCE_C_WORD), sptr_t(1));
        QCOMPARE(editor.send(message(Scintilla::Message::StyleGetUnderline), SCE_C_WORD),
                 sptr_t(1));
        scheme.syntax["keyword"].bold = false;
        scheme.syntax["keyword"].italic = false;
        scheme.syntax["keyword"].underline = false;
        editor.applyColorScheme(scheme);
        QCOMPARE(editor.send(message(Scintilla::Message::StyleGetBold), SCE_C_WORD), sptr_t(0));
        editor.configureLexerForPath(QStringLiteral("file.py"));
        QCOMPARE(editor.send(message(Scintilla::Message::StyleGetFore), SCE_P_WORD),
                 sptr_t(rgb(QColor(11, 22, 33))));
    }
    void terminalPalettePreservesScreenAndSelection() {
        TerminalSession session;
        session.resizeTerminal(6, 50);
        TerminalView view(session);
        session.feedOutput("\x1b[31mhello ANSI\x1b[0m\r\nsecond line");
        view.selectAll();
        const auto text = view.visibleText(), selected = view.selectedText();
        const auto cursor = session.cursorPosition();
        QSignalSpy started(&session, &TerminalSession::started);
        auto scheme = builtinColorScheme(SchemeKind::Dark);
        scheme.terminal["fg"] = QColor(1, 2, 3);
        scheme.terminal["bg"] = QColor(4, 5, 6);
        scheme.terminal["cursor"] = QColor(7, 8, 9);
        for (int i = 0; i < 16; ++i)
            scheme.terminal[QStringLiteral("ansi%1").arg(i)] = QColor(10 + i, 30 + i, 50 + i);
        view.applyColorScheme(scheme);
        QCOMPARE(started.count(), 0);
        QCOMPARE(view.visibleText(), text);
        QCOMPARE(view.selectedText(), selected);
        QCOMPARE(session.cursorPosition().row, cursor.row);
        QCOMPARE(session.cursorPosition().col, cursor.col);
        QCOMPARE(view.viewport()->palette().color(QPalette::Base), QColor(4, 5, 6));
        for (int i = 0; i < 16; ++i) {
            VTermColor color;
            vterm_color_indexed(&color, static_cast<uint8_t>(i));
            session.convertColorToRgb(color);
            QCOMPARE(QColor(color.rgb.red, color.rgb.green, color.rgb.blue),
                     QColor(10 + i, 30 + i, 50 + i));
        }
        auto cell = session.cellAt(0, 0);
        session.convertColorToRgb(cell.fg);
        QCOMPARE(QColor(cell.fg.rgb.red, cell.fg.rgb.green, cell.fg.rgb.blue), QColor(11, 31, 51));
    }

  private:
    QTemporaryDir settings_;
};
QTEST_MAIN(SchemeConsumerTest)
#include "SchemeConsumerTest.moc"
