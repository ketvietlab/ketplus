#include "ui/theme/import/JetBrainsImporter.h"
#include <QDir>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
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
QByteArray xmlValue(const QByteArray& name, const QList<QPair<QByteArray, QByteArray>>& fields) {
    QByteArray result = "<option name=\"" + name + "\"><value>";
    for (const auto& field : fields)
        result += "<option name=\"" + field.first + "\" value=\"" + field.second + "\"/>";
    return result + "</value></option>";
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
    void primaryAttributesWinRegardlessOfXmlOrder() {
        // IDE exports sort alphabetically: aliases occur both before and after the primary.
        QList<QByteArray> names{
            "DEFAULT_BLOCK_COMMENT",   "DEFAULT_CLASS_NAME",     "DEFAULT_CLASS_REFERENCE",
            "DEFAULT_DOC_COMMENT",     "DEFAULT_FUNCTION_CALL",  "DEFAULT_FUNCTION_DECLARATION",
            "DEFAULT_GLOBAL_VARIABLE", "DEFAULT_INSTANCE_FIELD", "DEFAULT_INSTANCE_METHOD",
            "DEFAULT_INTERFACE_NAME",  "DEFAULT_LINE_COMMENT",   "DEFAULT_LOCAL_VARIABLE",
            "DEFAULT_PARAMETER",       "DEFAULT_STATIC_FIELD",   "DEFAULT_STATIC_METHOD",
            "JAVA_METHOD_DECLARATION"};
        std::sort(names.begin(), names.end());
        const QSet<QByteArray> primary{"DEFAULT_CLASS_NAME", "DEFAULT_FUNCTION_DECLARATION",
                                       "DEFAULT_LINE_COMMENT", "DEFAULT_LOCAL_VARIABLE"};
        QTemporaryDir dir;
        const auto path = dir.filePath("ordered.icls");
        for (bool reversed : {false, true}) {
            if (reversed)
                std::reverse(names.begin(), names.end());
            QByteArray xml = "<scheme name=\"Alphabetical\" parent_scheme=\"Darcula\"><attributes>";
            for (const auto& name : names) {
                const bool main = primary.contains(name);
                QList<QPair<QByteArray, QByteArray>> fields{
                    {"FOREGROUND", main ? "A9B7C6" : "9876AA"}, {"FONT_TYPE", main ? "0" : "3"}};
                if (!main)
                    fields.append({{"EFFECT_TYPE", "2"}, {"EFFECT_COLOR", "ff0000"}});
                xml += xmlValue(name, fields);
            }
            QVERIFY(write(path, xml + "</attributes></scheme>"));
            const auto result = JetBrainsImporter::importFile(path);
            QVERIFY(result.ok());
            const auto& syntax = result.schemes.first().scheme.syntax;
            for (const auto& role : {"function", "variable", "type", "comment"}) {
                const auto style = syntax.value(QString::fromLatin1(role));
                QCOMPARE(style.fg.value(), QColor("#A9B7C6"));
                QCOMPARE(style.bold.value(), false);
                QCOMPARE(style.italic.value(), false);
                QCOMPARE(style.underline.value(), false);
            }
            for (const auto& name : names)
                if (!primary.contains(name))
                    QVERIFY(reportHas(result.report, QString::fromLatin1(name),
                                      ImportDisposition::Skipped));
        }
    }
    void secondaryAttributesSupplyMissingPrimaryValues() {
        QTemporaryDir dir;
        const auto path = dir.filePath("fallback.icls");
        const auto language =
            xmlValue("JAVA_METHOD_DECLARATION", {{"FOREGROUND", "ff0000"}, {"FONT_TYPE", "0"}});
        const auto secondary =
            xmlValue("DEFAULT_STATIC_METHOD", {{"FOREGROUND", "123456"}, {"FONT_TYPE", "2"}});
        const QByteArray inherited =
            "<option name=\"DEFAULT_FUNCTION_DECLARATION\" baseAttributes=\"BASE_METHOD\"/>";
        for (const auto& attributes :
             {language + inherited + secondary, secondary + inherited + language}) {
            QVERIFY(write(path, "<scheme name=\"Fallback\"><attributes>" + attributes +
                                    "</attributes></scheme>"));
            const auto result = JetBrainsImporter::importFile(path);
            QVERIFY(result.ok());
            const auto style = result.schemes.first().scheme.syntax.value("function");
            QCOMPARE(style.fg.value(), QColor("#123456"));
            QCOMPARE(style.bold.value(), false);
            QCOMPARE(style.italic.value(), true);
            QVERIFY(reportHas(result.report, "DEFAULT_FUNCTION_DECLARATION",
                              ImportDisposition::Skipped, "inheritance"));
            QVERIFY(reportHas(result.report, "DEFAULT_STATIC_METHOD",
                              ImportDisposition::Approximated, "Secondary"));
            QVERIFY(
                reportHas(result.report, "JAVA_METHOD_DECLARATION", ImportDisposition::Skipped));
        }
    }
    void underlineRequiresAnEffectColor_data() {
        QTest::addColumn<int>("effect");
        QTest::addColumn<QByteArray>("effectColor");
        QTest::addColumn<bool>("hasColor");
        QTest::addColumn<bool>("validColor");
        QTest::addColumn<bool>("colorFirst");
        for (int effect : {1, 2, 4, 5})
            for (const auto& state :
                 {QByteArray("missing"), QByteArray("empty"), QByteArray("invalid"),
                  QByteArray("black"), QByteArray("color")})
                for (bool first : {false, true}) {
                    const auto name =
                        QByteArray::number(effect) + '-' + state + (first ? "-before" : "-after");
                    const QByteArray value = state == "invalid" ? "not-a-color"
                                             : state == "black" ? "0"
                                             : state == "color" ? "ffaa00"
                                                                : "";
                    QTest::newRow(name.constData())
                        << effect << value << (state != "missing")
                        << (state == "black" || state == "color") << first;
                }
    }
    void underlineRequiresAnEffectColor() {
        QFETCH(int, effect);
        QFETCH(QByteArray, effectColor);
        QFETCH(bool, hasColor);
        QFETCH(bool, validColor);
        QFETCH(bool, colorFirst);
        QList<QPair<QByteArray, QByteArray>> fields{{"FOREGROUND", "123456"}};
        if (hasColor && colorFirst)
            fields.append({"EFFECT_COLOR", effectColor});
        fields.append({"EFFECT_TYPE", QByteArray::number(effect)});
        if (hasColor && !colorFirst)
            fields.append({"EFFECT_COLOR", effectColor});
        QTemporaryDir dir;
        const auto path = dir.filePath("effect.icls");
        QVERIFY(write(path, "<scheme name=\"Effects\"><attributes>" +
                                xmlValue("DEFAULT_KEYWORD", fields) + "</attributes></scheme>"));
        const auto result = JetBrainsImporter::importFile(path);
        QVERIFY(result.ok());
        const auto style = result.schemes.first().scheme.syntax.value("keyword");
        QCOMPARE(style.fg.value(), QColor("#123456"));
        QCOMPARE(style.underline.value(), validColor);
        QVERIFY(reportHas(result.report, "DEFAULT_KEYWORD/EFFECT_TYPE",
                          !validColor   ? ImportDisposition::Skipped
                          : effect == 1 ? ImportDisposition::Mapped
                                        : ImportDisposition::Approximated,
                          validColor ? QString{} : QStringLiteral("EFFECT_COLOR")));
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
