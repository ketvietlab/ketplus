#include "ui/theme/io/ThemeInput.h"
#include "ui/theme/vscode/VsCodeImporter.h"
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>
#include <miniz.h>
using namespace ketplus;
namespace {
QByteArray fixture(const QString& name) {
    QFile file(QString::fromUtf8(VSCODE_FIXTURE_DIR) + '/' + name);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool reported(const ImportReport& out, const QString& source, ImportDisposition kind,
              const QString& reason = {}) {
    for (const auto& entry : out.items)
        if (entry.disposition == kind && entry.sourcePath.contains(source) &&
            entry.reason.contains(reason))
            return true;
    return false;
}
QByteArray zip(const QList<QPair<QByteArray, QByteArray>>& entries) {
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_heap(&archive, 0, 0))
        return {};
    for (const auto& entry : entries)
        if (!mz_zip_writer_add_mem(&archive, entry.first.constData(), entry.second.constData(),
                                   size_t(entry.second.size()), MZ_BEST_COMPRESSION)) {
            mz_zip_writer_end(&archive);
            return {};
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
} // namespace
class VsCodeImportTest : public QObject {
    Q_OBJECT
  private slots:
    void darkIncludesStylesAndReport() {
        const auto path = QString::fromUtf8(VSCODE_FIXTURE_DIR) + "/dark.jsonc";
        const auto result = VsCodeImporter::importFile(path);
        QVERIFY(result.ok());
        QCOMPARE(result.schemes.size(), 1);
        const auto& s = result.schemes.first().scheme;
        QCOMPARE(s.kind, SchemeKind::Dark);
        QCOMPARE(s.name, QStringLiteral("Fixture Midnight"));
        QCOMPARE(s.editor["bg"], QColor("#101820"));
        QCOMPARE(s.editor["fg"], QColor("#CCDDEE"));
        QCOMPARE(s.ui["sidebar.bg"], QColor("#101820"));
        QCOMPARE(s.ui["input.fg"], QColor("#CCDDEE"));
        QCOMPARE(s.terminal["ansi1"], QColor(221, 68, 68, 221));
        QCOMPARE(s.diff["added"].alpha(), 128);
        QCOMPARE(s.syntax["keyword"].fg.value(), QColor("#BB88FF"));
        QVERIFY(!s.syntax["keyword"].bold.value());
        QVERIFY(!s.syntax["keyword"].italic.value());
        QVERIFY(!s.syntax["keyword"].underline.value());
        QCOMPARE(s.syntax["type"].fg.value(), QColor("#AABBCC"));
        QCOMPARE(s.syntax["variable"].fg.value(), QColor("#AABBCC"));
        QVERIFY(s.syntax["type"].bold.value());
        QVERIFY(s.syntax["type"].underline.value());
        QVERIFY(s.syntax["function"].underline.value());
        QVERIFY(s.syntax["comment"].italic.value());
        QCOMPARE(s.syntax["number"].fg.value(), QColor("#EEAA77"));
        QCOMPARE(s.syntax["operator"].fg.value(), QColor("#887766"));
        const auto semantic = s.metadata["vscodeSemanticTokenColors"].toObject();
        QCOMPARE(semantic["variable.readonly"].toString(), QStringLiteral("#00FF00"));
        QVERIFY(semantic.contains("function"));
        QVERIFY(
            reported(result.report, "semanticTokenColors", ImportDisposition::Skipped, "metadata"));
        QVERIFY(reported(result.report, "unknown.widget", ImportDisposition::Skipped));
        QVERIFY(reported(result.report, "source.js keyword", ImportDisposition::Skipped));
        QVERIFY(reported(result.report, "unknown.scope", ImportDisposition::Skipped));
        QVERIFY(reported(result.report, "strikethrough", ImportDisposition::Skipped));
        QVERIFY(reported(result.report, "background", ImportDisposition::Skipped));
        const auto canonical = ColorSchemeWriter::write(s);
        const auto read = ColorSchemeReader::read(canonical);
        QVERIFY(read.ok());
        QCOMPARE(read.scheme, s);
        QVERIFY(!canonical.contains(QString::fromUtf8(VSCODE_FIXTURE_DIR).toUtf8()));
        QCOMPARE(VsCodeImporter::importFile(path).schemes.first().scheme.id, s.id);
    }
    void lightAndFallbackChain() {
        const auto result =
            VsCodeImporter::importFile(QString::fromUtf8(VSCODE_FIXTURE_DIR) + "/light.json");
        QVERIFY(result.ok());
        const auto& s = result.schemes.first().scheme;
        QCOMPARE(s.kind, SchemeKind::Light);
        QCOMPARE(s.extends, QStringLiteral("builtin-light"));
        QCOMPARE(s.ui["list.selection"], QColor("#DDEEFF"));
        QVERIFY(reported(result.report, "inactiveSelectionBackground",
                         ImportDisposition::Approximated, "fallback"));
        QCOMPARE(s.terminal["fg"], QColor("#112233"));
        QTemporaryDir dir;
        const auto path = dir.filePath("chain.json");
        QVERIFY(write(path,
                      "{\"colors\":{\"input.foreground\":\"#123456\",\"foreground\":\"#223344\","
                      "\"editor.foreground\":\"#334455\",\"list.activeSelectionBackground\":\"#"
                      "111111\",\"list.inactiveSelectionBackground\":\"#222222\"}}"));
        const auto explicitResult = VsCodeImporter::importFile(path);
        QVERIFY(explicitResult.ok());
        QCOMPARE(explicitResult.schemes.first().scheme.ui["input.fg"], QColor("#123456"));
        QCOMPARE(explicitResult.schemes.first().scheme.ui["list.selection"], QColor("#111111"));
    }
    void ansi16() {
        const QStringList names{"Black",       "Red",           "Green",       "Yellow",
                                "Blue",        "Magenta",       "Cyan",        "White",
                                "BrightBlack", "BrightRed",     "BrightGreen", "BrightYellow",
                                "BrightBlue",  "BrightMagenta", "BrightCyan",  "BrightWhite"};
        QJsonObject colors;
        for (int i = 0; i < names.size(); ++i)
            colors["terminal.ansi" + names[i]] = QColor(i + 1, i + 2, i + 3).name();
        QTemporaryDir dir;
        const auto path = dir.filePath("ansi.json");
        QVERIFY(write(path, QJsonDocument(QJsonObject{{"colors", colors}}).toJson()));
        const auto result = VsCodeImporter::importFile(path);
        QVERIFY(result.ok());
        for (int i = 0; i < 16; ++i)
            QCOMPARE(result.schemes.first().scheme.terminal["ansi" + QString::number(i)],
                     QColor(i + 1, i + 2, i + 3));
    }
    void standardTmThemeAndRuleOrder() {
        const auto result =
            VsCodeImporter::importFile(QString::fromUtf8(VSCODE_FIXTURE_DIR) + "/plist-theme.json");
        QVERIFY(result.ok());
        const auto& s = result.schemes.first().scheme;
        QCOMPARE(s.editor["fg"], QColor("#EEEEEE"));
        QCOMPARE(s.editor["bg"], QColor("#151515"));
        QCOMPARE(s.editor["caret"], QColor("#FFAA11"));
        QCOMPARE(s.editor["selection"], QColor("#334455"));
        QCOMPARE(s.editor["lineHighlight"], QColor("#222233"));
        QCOMPARE(s.editor["gutterFg"], QColor("#667788"));
        QCOMPARE(s.editor["gutterBg"], QColor("#151515"));
        QCOMPARE(s.ui["sidebar.bg"], QColor("#151515"));
        QCOMPARE(s.ui["chrome.pageBackground"], QColor("#151515"));
        QCOMPARE(s.ui["tab.activeBg"], QColor("#151515"));
        QCOMPARE(s.terminal["bg"], QColor("#151515"));
        QVERIFY(
            reported(result.report, "invisibles", ImportDisposition::Skipped, "no corresponding"));

        QCOMPARE(s.syntax["string"].fg.value(), QColor("#55AA66"));
        QVERIFY(s.syntax["string"].italic.value());
        QVERIFY(s.syntax["string"].underline.value());
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("parent.json"),
                      "{\"tokenColors\":[{\"settings\":{\"foreground\":\"#111111\"}}]}"));
        QVERIFY(write(dir.filePath("child.json"), "{\"include\":\"parent.json\",\"tokenColors\":[{"
                                                  "\"settings\":{\"foreground\":\"#222222\"}}]}"));
        auto child = VsCodeImporter::importFile(dir.filePath("child.json"));
        QVERIFY(child.ok());
        QCOMPARE(child.schemes.first().scheme.editor["fg"], QColor("#222222"));
        QVERIFY(write(dir.filePath("syntax.tmTheme"), fixture("syntax.tmTheme")));
        QVERIFY(write(dir.filePath("parent.json"), "{\"tokenColors\":\"syntax.tmTheme\"}"));
        QVERIFY(
            write(dir.filePath("child.json"),
                  "{\"include\":\"parent.json\",\"colors\":{\"editor.foreground\":\"#AABBCC\"},"
                  "\"tokenColors\":[{\"scope\":\"string\",\"settings\":{\"fontStyle\":\"\"}}]}"));
        child = VsCodeImporter::importFile(dir.filePath("child.json"));
        QVERIFY(child.ok());
        QCOMPARE(child.schemes.first().scheme.editor["fg"], QColor("#AABBCC"));
        QCOMPARE(child.schemes.first().scheme.syntax["string"].fg.value(), QColor("#55AA66"));
        QVERIFY(!child.schemes.first().scheme.syntax["string"].italic.value());
    }
    void vsixVariants() {
        const QByteArray manifest =
            "{\"license\":\"MPL-2.0\",\"contributes\":{\"themes\":[{\"label\":\"Dark "
            "variant\",\"uiTheme\":\"vs-dark\",\"path\":\"./dark.jsonc\"},{\"label\":\"Light "
            "variant\",\"uiTheme\":\"vs\",\"path\":\"light.json\"},{\"label\":\"Contrast\","
            "\"uiTheme\":\"hc-black\",\"path\":\"dark.jsonc\"}]}}";
        QTemporaryDir dir;
        const auto path = dir.filePath("themes.vsix");
        QVERIFY(write(path, zip({{"extension/package.json", manifest},
                                 {"extension/dark.jsonc", fixture("dark.jsonc")},
                                 {"extension/parent.jsonc", fixture("parent.jsonc")},
                                 {"extension/light.json", fixture("light.json")},
                                 {"extension/never-run.js", "throw new Error('inert fixture')"}})));
        const auto result = VsCodeImporter::importVsix(path);
        QVERIFY(result.ok());
        QCOMPARE(result.schemes.size(), 3);
        QCOMPARE(result.schemes[0].scheme.kind, SchemeKind::Dark);
        QCOMPARE(result.schemes[1].scheme.kind, SchemeKind::Light);
        QCOMPARE(result.schemes[2].scheme.kind, SchemeKind::HighContrast);
        QCOMPARE(result.schemes[2].scheme.extends, QStringLiteral("builtin-dark"));
        QVERIFY(result.schemes[0].scheme.id != result.schemes[2].scheme.id);
        QCOMPARE(result.schemes[0].scheme.metadata["license"].toString(),
                 QStringLiteral("MPL-2.0"));
        for (const auto& variant : result.schemes)
            QVERIFY(ColorSchemeReader::read(ColorSchemeWriter::write(variant.scheme)).ok());
        QCOMPARE(VsCodeImporter::importFile(path).schemes.size(), 3);
        QVERIFY(
            reported(result.report, "package.json", ImportDisposition::Skipped, "never loaded"));
        QVERIFY(
            write(path, zip({{"extension/package.json", "{\"contributes\":{\"themes\":[{\"path\":"
                                                        "\"https://example.test/t.json\"}]}}"}})));
        QVERIFY(!VsCodeImporter::importVsix(path).ok());
        QVERIFY(write(
            path, zip({{"extension/package.json", "{\"contributes\":{\"themes\":[{\"label\":\"%"
                                                  "localized.name%\",\"path\":\"light.json\"}]}}"},
                       {"extension/light.json", fixture("light.json")}})));
        const auto localized = VsCodeImporter::importVsix(path);
        QVERIFY(localized.ok());
        QCOMPARE(localized.schemes.first().scheme.name, QStringLiteral("Fixture Paper"));
        QVERIFY(reported(localized.report, "/name", ImportDisposition::Approximated, "Localized"));
        QVERIFY(write(path, zip({{"../escape", "{}"}})));
        QVERIFY(!VsCodeImporter::importVsix(path).ok());
    }
    void packageMetadataStillRespectsCanonicalLimit() {
        QTemporaryDir dir;
        const auto path = dir.filePath("oversized.vsix");
        const QJsonObject theme{
            {"semanticTokenColors", QJsonObject{{"variable", QString(3000000, u'x')}}}};
        const QJsonObject manifest{
            {"license", QString(3000000, u'x')},
            {"contributes",
             QJsonObject{{"themes", QJsonArray{QJsonObject{{"path", "theme.json"}}}}}}};
        QVERIFY(write(
            path,
            zip({{"extension/package.json", QJsonDocument(manifest).toJson(QJsonDocument::Compact)},
                 {"extension/theme.json", QJsonDocument(theme).toJson(QJsonDocument::Compact)}})));
        const auto result = VsCodeImporter::importVsix(path);
        QVERIFY(!result.ok());
        QVERIFY(result.schemes.isEmpty());
        QVERIFY(!result.report.diagnostics.isEmpty());
    }
    void failuresAndSharedBudget() {
        QTemporaryDir dir;
        const auto path = dir.filePath("bad.json");
        for (const auto& bytes : {QByteArray("{\"include\":\"bad.json\"}"),
                                  QByteArray("{\"include\":\"../outside.json\"}"),
                                  QByteArray("{\"tokenColors\":\"/etc/passwd\"}"),
                                  QByteArray("{\"tokenColors\":false}"),
                                  QByteArray("{\"colors\":[]}"), QByteArray("{\n\"colors\":@}")}) {
            QVERIFY(write(path, bytes));
            const auto result = VsCodeImporter::importFile(path);
            QVERIFY(!result.ok());
            QVERIFY(result.schemes.isEmpty());
            QVERIFY(!result.report.diagnostics.isEmpty());
            if (bytes.contains("include"))
                QVERIFY(!result.report.diagnostics.first().message.contains("line 0"));
        }
        QVERIFY(write(dir.filePath("evil.tmTheme"),
                      "<!DOCTYPE plist [<!ENTITY x SYSTEM "
                      "'file:///etc/passwd'>]><plist><dict>&x;</dict></plist>"));
        QVERIFY(write(path, "{\"tokenColors\":\"evil.tmTheme\"}"));
        QVERIFY(!VsCodeImporter::importFile(path).ok());
        QVERIFY(write(path, "{}"));
        ket::theme::io::IncludeResolver resolver(dir.path());
        QVector<ket::theme::io::JsonSource> chain;
        ket::theme::io::Error error;
        quint64 budget = 4;
        QVERIFY(resolver.loadJsonChain("bad.json", &chain, &error, &budget));
        QCOMPARE(budget, quint64(2));
        QVERIFY(resolver.loadJsonChain("bad.json", &chain, &error, &budget));
        QCOMPARE(budget, quint64(0));
        QVERIFY(!resolver.loadJsonChain("bad.json", &chain, &error, &budget));
        QVERIFY(chain.isEmpty());
    }
};
QTEST_GUILESS_MAIN(VsCodeImportTest)
#include "VsCodeImportTest.moc"
