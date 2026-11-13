#include "ui/theme/import/JetBrainsImporter.h"
#include <QDir>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>
#include <miniz.h>

using namespace ketplus;
namespace {
QByteArray fixture(const QString& name) {
    QFile file(QString::fromUtf8(JETBRAINS_FIXTURE_DIR) + '/' + name);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool reportHas(const ImportReport& report, const QString& source, ImportDisposition disposition,
               const QString& reason = {}) {
    for (const auto& item : report.items)
        if (item.sourcePath.contains(source) && item.disposition == disposition &&
            item.reason.contains(reason))
            return true;
    return false;
}
QByteArray zip(const QList<QPair<QByteArray, QByteArray>>& entries) {
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_heap(&archive, 0, 0))
        return {};
    for (const auto& entry : entries) {
        if (!mz_zip_writer_add_mem(&archive, entry.first.constData(), entry.second.constData(),
                                   size_t(entry.second.size()), MZ_BEST_COMPRESSION)) {
            mz_zip_writer_end(&archive);
            return {};
        }
    }
    void* buffer = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&archive, &buffer, &size)) {
        mz_zip_writer_end(&archive);
        return {};
    }
    QByteArray result(static_cast<const char*>(buffer), qsizetype(size));
    mz_free(buffer);
    mz_zip_writer_end(&archive);
    return result;
}
QByteArray xmlAttribute(const QByteArray& name, const QByteArray& property,
                        const QByteArray& value) {
    return "<option name=\"" + name + "\"><value><option name=\"" + property + "\" value=\"" +
           value + "\"/></value></option>";
}
} // namespace
class JetBrainsImportTest : public QObject {
    Q_OBJECT
  private slots:
    void darkEditor() {
        const auto result =
            JetBrainsImporter::importFile(QString::fromUtf8(JETBRAINS_FIXTURE_DIR) + "/dark.icls");
        QVERIFY(result.ok());
        QCOMPARE(result.schemes.size(), 1);
        const auto& variant = result.schemes.first();
        const auto& s = variant.scheme;
        QCOMPARE(s.kind, SchemeKind::Dark);
        QCOMPARE(s.extends, QStringLiteral("builtin-dark"));
        QCOMPARE(s.editor["bg"], QColor("#101820"));
        QCOMPARE(s.editor["fg"], QColor("#ddeeff"));
        QCOMPARE(s.editor["lineHighlight"], QColor("#202530"));
        QCOMPARE(s.editor["selection"].alpha(), 128);
        QCOMPARE(s.terminal["bg"], QColor("#000000"));
        QCOMPARE(s.terminal["ansi1"], QColor("#cc4444"));
        QCOMPARE(s.terminal["ansi12"], QColor("#6688ff"));
        QCOMPARE(s.diff["added"], QColor("#224433"));
        QCOMPARE(s.diff["removed"], QColor("#442233"));
        QCOMPARE(s.syntax["keyword"].fg.value(), QColor("#bb88ff"));
        QVERIFY(s.syntax["keyword"].bold.value());
        QVERIFY(s.syntax["keyword"].italic.value());
        QVERIFY(s.syntax["comment"].underline.value());
        QCOMPARE(s.syntax["type"].fg.value(), QColor("#ddaaaa"));
        QVERIFY(!s.syntax["string"].bold.value());
        QVERIFY(!s.syntax["string"].italic.value());
        QVERIFY(!s.syntax["string"].underline.value());
        QVERIFY(reportHas(result.report, "parent_scheme", ImportDisposition::Approximated));
        QVERIFY(reportHas(result.report, "JAVA_KEYWORD", ImportDisposition::Skipped));
        QVERIFY(reportHas(result.report, "KOTLIN_CLASS", ImportDisposition::Approximated));
        QVERIFY(reportHas(result.report, "KOTLIN_SMART_CAST", ImportDisposition::Skipped));
        QVERIFY(reportHas(result.report, "EFFECT_TYPE", ImportDisposition::Approximated));
        QVERIFY(reportHas(result.report, "EFFECT_COLOR", ImportDisposition::Skipped));
        QVERIFY(reportHas(result.report, "EDITOR_FONT_SIZE", ImportDisposition::Skipped));
        QVERIFY(reportHas(result.report, "DEFAULT_CONSTANT", ImportDisposition::Skipped,
                          "inheritance"));
        const auto canonical = ColorSchemeWriter::write(s);
        const auto reread = ColorSchemeReader::read(canonical);
        QVERIFY(reread.ok());
        QCOMPARE(reread.scheme, s);
        QVERIFY(!canonical.contains(QString::fromUtf8(JETBRAINS_FIXTURE_DIR).toUtf8()));
    }
    void lightEditorAndInference() {
        QTemporaryDir dir;
        const auto path = dir.filePath("light.icls");
        auto bytes = fixture("light.icls");
        QVERIFY(!bytes.isEmpty());
        QVERIFY(write(path, bytes));
        auto result = JetBrainsImporter::importFile(path);
        QVERIFY(result.ok());
        QCOMPARE(result.schemes.first().scheme.kind, SchemeKind::Light);
        QVERIFY(!result.schemes.first().scheme.syntax["keyword"].bold.value());
        QVERIFY(!result.schemes.first().scheme.syntax["keyword"].underline.value());
        QVERIFY(reportHas(result.report, "DEFAULT_STRING/EFFECT_TYPE", ImportDisposition::Skipped));
        bytes.replace("fafafa", "000001");
        QVERIFY(write(path, bytes));
        result = JetBrainsImporter::importFile(path);
        QVERIFY(result.ok());
        QCOMPARE(result.schemes.first().scheme.kind, SchemeKind::Dark);
        QCOMPARE(result.schemes.first().scheme.extends, QStringLiteral("builtin-dark"));
        QVERIFY(write(path, "<scheme name=\"Parent\" parent_scheme=\"Darcula\"/>"));
        result = JetBrainsImporter::importFile(path);
        QVERIFY(result.ok());
        QCOMPARE(result.schemes.first().scheme.kind, SchemeKind::Dark);
    }
    void explicitAttributeMappings() {
        const QList<QPair<QByteArray, QString>> expected{
            {"DEFAULT_KEYWORD", "keyword"},
            {"DEFAULT_STRING", "string"},
            {"DEFAULT_NUMBER", "number"},
            {"DEFAULT_LINE_COMMENT", "comment"},
            {"DEFAULT_FUNCTION_DECLARATION", "function"},
            {"DEFAULT_METADATA", "attribute"},
            {"DEFAULT_CONSTANT", "constant"},
            {"DEFAULT_LOCAL_VARIABLE", "variable"},
            {"DEFAULT_OPERATION_SIGN", "operator"},
            {"DEFAULT_CLASS_NAME", "type"},
            {"DEFAULT_CLASS_REFERENCE", "type"}};
        QTemporaryDir dir;
        const auto path = dir.filePath("mapping.icls");
        for (const auto& mapping : expected) {
            QVERIFY(write(path, "<scheme name=\"Mapping\"><attributes>" +
                                    xmlAttribute(mapping.first, "FOREGROUND", "123456") +
                                    "</attributes></scheme>"));
            const auto result = JetBrainsImporter::importFile(path);
            QVERIFY(result.ok());
            QCOMPARE(result.schemes.first().scheme.syntax[mapping.second].fg.value(),
                     QColor("#123456"));
        }
        const QList<QByteArray> ansi{
            "BLACK",        "RED",           "GREEN",       "YELLOW",         "BLUE",
            "MAGENTA",      "CYAN",          "GRAY",        "DARKGRAY",       "RED_BRIGHT",
            "GREEN_BRIGHT", "YELLOW_BRIGHT", "BLUE_BRIGHT", "MAGENTA_BRIGHT", "CYAN_BRIGHT",
            "WHITE"};
        for (int i = 0; i < ansi.size(); ++i) {
            const auto key = "CONSOLE_" + ansi[i] + "_OUTPUT";
            QVERIFY(write(path, "<scheme name=\"ANSI\"><attributes>" +
                                    xmlAttribute(key, "FOREGROUND", "223344") +
                                    "</attributes></scheme>"));
            const auto result = JetBrainsImporter::importFile(path);
            QVERIFY(result.ok());
            QCOMPARE(result.schemes.first().scheme.terminal["ansi" + QString::number(i)],
                     QColor("#223344"));
        }
    }
    void uiThemeWithEditor() {
        const auto path = QString::fromUtf8(JETBRAINS_FIXTURE_DIR) + "/sample.theme.json";
        const auto result = JetBrainsImporter::importFile(path);
        QVERIFY(result.ok());
        const auto& s = result.schemes.first().scheme;
        QCOMPARE(s.name, QStringLiteral("Fixture UI"));
        QCOMPARE(s.kind, SchemeKind::Dark);
        QCOMPARE(s.ui["chrome.panelBackground"].alpha(), 128);
        QCOMPARE(s.ui["chrome.textMain"], QColor("#ddeeff"));
        QCOMPARE(s.ui["input.bg"], QColor("#102030"));
        QCOMPARE(s.ui["tab.activeFg"], QColor("#abcdef"));
        QCOMPARE(s.ui["list.selection"], QColor("#445566"));
        QCOMPARE(s.editor["bg"], QColor("#101820"));
        QVERIFY(reportHas(result.report, "CustomWidget", ImportDisposition::Skipped));
        QVERIFY(reportHas(result.report, "icons", ImportDisposition::Skipped));
        QVERIFY(reportHas(result.report, "colors/alias", ImportDisposition::Mapped));
        QVERIFY(reportHas(result.report, "colors/unused", ImportDisposition::Skipped));
        QVERIFY(ColorSchemeReader::read(ColorSchemeWriter::write(s)).ok());
        QCOMPARE(JetBrainsImporter::importFile(path).schemes.first().scheme.id, s.id);
    }
    void jarVariantsAndRootResource() {
        QTemporaryDir dir;
        const auto path = dir.filePath("fixture.jar");
        auto ui = fixture("sample.theme.json");
        ui.replace("dark.icls", "/editor/Dark.xml");
        QVERIFY(
            write(path, zip({{"themes/ui.theme.json", ui},
                             {"editor/Dark.xml", fixture("dark.icls")},
                             {"Light.icls", fixture("light.icls")},
                             {"META-INF/plugin.xml", "<idea-plugin/>"},
                             {"unsafe.xml", "<!DOCTYPE x SYSTEM 'https://example.test/test'><x/>"},
                             {"plugin/DoNotExecute.class", "inert fake bytecode fixture"}})));
        const auto result = JetBrainsImporter::importArchive(path);
        QVERIFY(result.ok());
        QCOMPARE(result.schemes.size(), 2);
        QSet<QString> ids;
        for (const auto& variant : result.schemes) {
            ids.insert(variant.scheme.id);
            QVERIFY(ColorSchemeReader::read(ColorSchemeWriter::write(variant.scheme)).ok());
            QVERIFY(!variant.sourcePath.startsWith('/'));
        }
        QCOMPARE(ids.size(), 2);
        QVERIFY(reportHas(result.report, "plugin.xml", ImportDisposition::Skipped));
        QVERIFY(reportHas(result.report, "unsafe.xml", ImportDisposition::Skipped));
        QVERIFY(
            reportHas(result.report, "fixture.jar", ImportDisposition::Skipped, "never executed"));
        QCOMPARE(JetBrainsImporter::importFile(path).schemes.size(), 2);
        QVERIFY(write(path, zip({{"../escape.icls", fixture("dark.icls")}})));
        QVERIFY(!JetBrainsImporter::importArchive(path).ok());
    }
    void errorsAndBoundary() {
        QTemporaryDir dir;
        QTemporaryDir outside;
        const auto path = dir.filePath("bad.theme.json");
        QVERIFY(write(outside.filePath("secret.icls"), fixture("dark.icls")));
        for (const auto& reference :
             {QStringLiteral("../secret.icls"), outside.filePath("secret.icls"),
              QStringLiteral("https://example.test/a"), QStringLiteral("missing.icls"),
              QStringLiteral("//host/a")}) {
            const auto bytes = "{\"name\":\"Bad\",\"editorScheme\":\"" + reference.toUtf8() + "\"}";
            QVERIFY(write(path, bytes));
            const auto result = JetBrainsImporter::importFile(path);
            QVERIFY(!result.ok());
            QVERIFY(result.schemes.isEmpty());
            QVERIFY(!result.report.diagnostics.isEmpty());
        }
        QVERIFY(write(
            path,
            "{\"name\":\"Palette "
            "cycle\",\"colors\":{\"a\":\"b\",\"b\":\"a\"},\"ui\":{\"Panel.background\":\"a\"}}"));
        auto result = JetBrainsImporter::importFile(path);
        QVERIFY(result.ok());
        QVERIFY(result.schemes.first().scheme.ui.isEmpty());
        QVERIFY(!reportHas(result.report, "colors/a", ImportDisposition::Mapped));
        QVERIFY(!reportHas(result.report, "colors/b", ImportDisposition::Mapped));
        QVERIFY(reportHas(result.report, "Panel.background", ImportDisposition::Skipped, "cyclic"));
        QVERIFY(write(path, "{\"dark\":\"yes\"}"));
        QVERIFY(!JetBrainsImporter::importFile(path).ok());
        QVERIFY(write(path, "{\n\"name\":@}"));
        result = JetBrainsImporter::importFile(path);
        QVERIFY(!result.ok());
        QVERIFY(result.report.diagnostics.first().message.contains("line 2"));
        const auto xml = dir.filePath("bad.icls");
        QVERIFY(write(xml, "<!DOCTYPE scheme [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><scheme "
                           "name=\"Unsafe\">&x;</scheme>"));
        QVERIFY(!JetBrainsImporter::importFile(xml).ok());
        QVERIFY(write(xml, "<unrelated/>"));
        QVERIFY(!JetBrainsImporter::importFile(xml).ok());
        QVERIFY(!JetBrainsImporter::importFile(outside.filePath("secret.icls"), dir.path()).ok());
        QVERIFY(write(
            path, "{\"name\":\"Root resource\",\"dark\":false,\"editorScheme\":\"/light.icls\"}"));
        QVERIFY(write(dir.filePath("light.icls"), fixture("light.icls")));
        result = JetBrainsImporter::importFile(path);
        QVERIFY(result.ok());
        QCOMPARE(result.schemes.first().scheme.kind, SchemeKind::Light);
    }
};
QTEST_GUILESS_MAIN(JetBrainsImportTest)
#include "JetBrainsImportTest.moc"
