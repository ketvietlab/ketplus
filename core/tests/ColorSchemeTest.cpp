#include "ui/theme/ColorScheme.h"
#include "KetSyntax.h"
#include "KetTokens.h"
#include "ui/theme/ColorSchemeCatalog.h"
#include "ui/theme/ColorSchemeTokens.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

using namespace ketplus;
namespace {
QByteArray sparse() {
    return R"({"schemaVersion":1,"id":"test-scheme","name":"Test","kind":"highContrast","extends":"builtin-dark"})";
}
ThemePalette legacyPalette(ket::Theme theme) {
    const auto token = [theme](const char* name) {
        QColor color = ket::Tokens::color(QString::fromLatin1(name), theme);
        if (color.alpha() < 255) {
            const QColor base = ket::Tokens::color(u"panel-bg", theme);
            const qreal alpha = color.alphaF();
            color = QColor::fromRgbF(color.redF() * alpha + base.redF() * (1 - alpha),
                                     color.greenF() * alpha + base.greenF() * (1 - alpha),
                                     color.blueF() * alpha + base.blueF() * (1 - alpha));
        }
        return color.name(QColor::HexRgb).toUpper();
    };
    const auto syntax = ket::syntaxRecipe(theme);
    const auto hex = [](const QColor& color) { return color.name(QColor::HexRgb).toUpper(); };
    return {

        .dark = theme == ket::Theme::Dark,
        .appBackground = token("page-bg"),
        .pageBackground = token("page-bg"),
        .sidebarBackground = token("sidebar-bg"),
        .panelBackground = token("panel-bg"),
        .panelSubtle = token("panel-bg-subtle"),
        .surfaceRaised = token("surface-raised"),
        .surfaceHover = token("surface-hover"),
        .textMain = token("text-main"),
        .textSecondary = token("text-secondary"),
        .textMuted = token("text-muted"),
        .textDisabled = token("text-disabled"),
        .border = token("panel-border"),
        .borderStrong = token("border-strong"),
        .interactiveBackground = token("interactive-bg"),
        .interactiveHover = token("interactive-hover"),
        .interactiveActive = token("interactive-active"),
        .accent = token("accent"),
        .accentHover = token("accent-hover"),
        .accentActive = token("accent-active"),
        .accentSubtle = token("accent-subtle"),
        .accentMuted = token("accent-muted"),
        .focus = token("color-focus"),
        .tabActiveText = token("tab-active-text"),
        .accentText = token("accent-text"),
        .actionPrimary = token("action-primary-bg"),
        .actionPrimaryHover = token("action-primary-hover"),
        .actionPrimaryActive = token("action-primary-active"),
        .actionPrimaryInk = token("action-primary-ink"),
        .positive = token("positive"),
        .warning = token("warning"),
        .danger = token("danger"),
        .info = token("info"),
        .syntaxKeyword = hex(syntax.keyword),
        .syntaxString = hex(syntax.string),
        .syntaxNumber = hex(syntax.number),
        .syntaxType = hex(syntax.type),
        .syntaxComment = hex(syntax.comment),
        .syntaxAttribute = hex(syntax.attribute),
    };
}
} // namespace
class ColorSchemeTest : public QObject {
    Q_OBJECT
  private slots:
    void colors_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QColor>("expected");
        QTest::newRow("short") << QStringLiteral("#a3f") << QColor(170, 51, 255);
        QTest::newRow("rgb") << QStringLiteral("#1234aB") << QColor(18, 52, 171);
        QTest::newRow("rgba") << QStringLiteral("#1234ab80") << QColor(18, 52, 171, 128);
        QTest::newRow("transparent") << QStringLiteral("#abcdef00") << QColor(171, 205, 239, 0);
    }
    void colors() {
        QFETCH(QString, text);
        QFETCH(QColor, expected);
        QCOMPARE(ColorSchemeReader::parseColor(text), std::optional<QColor>(expected));
    }
    void rejectsColors() {
        for (const auto& value :
             {"red", "#abcd", "#12", "#123456789", "#12345z", " #fff", "#fff\n"})
            QVERIFY(!ColorSchemeReader::parseColor(QString::fromLatin1(value)));
    }
    void roundTripPreservesUnknownAndStyles() {
        auto object = QJsonDocument::fromJson(sparse()).object();
        object.insert("future", QJsonObject{{"flag", true}});
        object.insert("ui", QJsonObject{{"sidebar", QJsonObject{{"bg", "#a3f"}, {"future", 42}}},
                                        {"newGroup", QJsonObject{{"x", "unparsed"}}}});
        object.insert("editor", QJsonObject{{"bg", "#12345678"}, {"future", QJsonArray{1, 2}}});
        object.insert("syntax", QJsonObject{{"keyword", QJsonObject{{"fg", "#abc"},
                                                                    {"bold", false},
                                                                    {"italic", true},
                                                                    {"underline", false},
                                                                    {"future", 42}}},
                                            {"newRole", QJsonObject{{"fg", "unknown"}}}});
        object.insert("terminal", QJsonObject{{"ansi15", "#123"}, {"future", true}});
        object.insert("diff", QJsonObject{{"added", "#456"}, {"future", false}});
        object.insert("metadata", QJsonObject{{"sourceApp", "VS Code"},
                                              {"sourceFile", "theme.json"},
                                              {"license", "MIT"},
                                              {"custom", 7}});
        const auto first = ColorSchemeReader::read(QJsonDocument(object).toJson());
        QVERIFY(first.ok());
        QVERIFY(!first.diagnostics.isEmpty());
        const auto bytes = ColorSchemeWriter::write(first.scheme);
        const auto second = ColorSchemeReader::read(bytes);
        QVERIFY(second.ok());
        QVERIFY(first.scheme == second.scheme);
        const auto output = QJsonDocument::fromJson(bytes).object();
        QCOMPARE(output.value("future"), object.value("future"));
        QCOMPARE(output.value("ui").toObject().value("sidebar").toObject().value("future").toInt(),
                 42);
        QCOMPARE(
            output.value("syntax").toObject().value("keyword").toObject().value("future").toInt(),
            42);
        QCOMPARE(second.scheme.editor.value("bg"), QColor(18, 52, 86, 120));
    }
    void diagnostics_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<QString>("path");
        QTest::newRow("jsonc") << QByteArray("{/*comment*/}") << QStringLiteral("$");
        QTest::newRow("root") << QByteArray("[]") << QStringLiteral("$");
        for (const auto& pair :
             {std::pair{"schemaVersion", QJsonValue(2)}, std::pair{"id", QJsonValue("../bad")},
              std::pair{"name", QJsonValue(1)}, std::pair{"kind", QJsonValue("other")},
              std::pair{"extends", QJsonValue("recursive")}, std::pair{"ui", QJsonValue(false)},
              std::pair{"metadata", QJsonValue(false)}}) {
            auto obj = QJsonDocument::fromJson(sparse()).object();
            obj.insert(pair.first, pair.second);
            QTest::newRow(pair.first) << QJsonDocument(obj).toJson()
                                      << QStringLiteral("$.") + QString::fromLatin1(pair.first);
        }
        auto obj = QJsonDocument::fromJson(sparse()).object();
        obj.insert("editor", QJsonObject{{"bg", "#badcolor"}});
        QTest::newRow("color") << QJsonDocument(obj).toJson() << QStringLiteral("$.editor.bg");
        obj = QJsonDocument::fromJson(sparse()).object();
        obj.insert("syntax", QJsonObject{{"keyword", QJsonObject{{"bold", "yes"}}}});
        QTest::newRow("flag") << QJsonDocument(obj).toJson()
                              << QStringLiteral("$.syntax.keyword.bold");
    }
    void diagnostics() {
        QFETCH(QByteArray, bytes);
        QFETCH(QString, path);
        const auto result = ColorSchemeReader::read(bytes);
        QVERIFY(!result.ok());
        QVERIFY(
            std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& d) {
                return d.path == path && d.severity == DiagnosticSeverity::Error;
            }));
    }
    void fallbackAndExplicitReset() {
        auto scheme = ColorSchemeReader::read(sparse()).scheme;
        scheme.syntax["keyword"].bold = false;
        scheme.syntax["keyword"].italic = true;
        scheme.editor["bg"] = QColor(1, 2, 3, 4);
        const auto resolved = resolveColorScheme(scheme);
        const auto builtin = builtinColorScheme(SchemeKind::Dark);
        QCOMPARE(resolved.ui, builtin.ui);
        QCOMPARE(resolved.editor.value("fg"), builtin.editor.value("fg"));
        QCOMPARE(resolved.editor.value("bg"), QColor(1, 2, 3, 4));
        QCOMPARE(resolved.syntax.value("keyword").bold, std::optional<bool>(false));
        QCOMPARE(resolved.syntax.value("keyword").italic, std::optional<bool>(true));
        for (const auto* colors :
             {&resolved.ui, &resolved.editor, &resolved.terminal, &resolved.diff})
            for (const auto& color : *colors)
                QVERIFY(color.isValid());
        for (const auto& style : resolved.syntax) {
            QVERIFY(style.fg && style.fg->isValid());
            QVERIFY(style.bold.has_value() && style.italic.has_value() &&
                    style.underline.has_value());
        }
        for (int i = 0; i < 16; ++i)
            QVERIFY(resolved.terminal.value(QStringLiteral("ansi%1").arg(i)).isValid());
    }
    void highContrastKeepsBaseBrightness() {
        auto scheme = ColorSchemeReader::read(sparse()).scheme;
        QVERIFY(colorSchemePalette(scheme).dark);
        scheme.extends = QStringLiteral("builtin-light");
        QVERIFY(!colorSchemePalette(scheme).dark);
        QCOMPARE(resolveColorScheme(scheme).kind, SchemeKind::HighContrast);
    }
    void builtinParity() {
        for (auto kind : {SchemeKind::Light, SchemeKind::Dark}) {
            const auto builtin = builtinColorScheme(kind);
            const auto reread = ColorSchemeReader::read(ColorSchemeWriter::write(builtin));
            QVERIFY(reread.ok());
            QVERIFY(reread.scheme == builtin);
            QVERIFY(
                colorSchemePalette(builtin) ==
                legacyPalette(kind == SchemeKind::Light ? ket::Theme::Light : ket::Theme::Dark));
        }
    }
    void catalogLifecycle() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ColorSchemeCatalog catalog(dir.path());
        QCOMPARE(catalog.list().size(), 2);
        auto scheme = ColorSchemeReader::read(sparse()).scheme;
        scheme.id.clear();
        const auto added = catalog.add(scheme);
        QVERIFY(added.success);
        QVERIFY(!added.scheme.id.isEmpty());
        ColorSchemeCatalog reopened(dir.path());
        QVERIFY(reopened.load(added.scheme.id).scheme == added.scheme);
        QVERIFY(!reopened.add(added.scheme).success);
        QCOMPARE(reopened.list().size(), 3);
        auto copy = reopened.duplicate(added.scheme.id, "Copy");
        QVERIFY(copy.success);
        QVERIFY(copy.scheme.id != added.scheme.id);
        QCOMPARE(copy.scheme.name, QStringLiteral("Copy"));
        QVERIFY(reopened.remove(added.scheme.id));
        const auto missing = reopened.load(added.scheme.id, SchemeKind::Dark);
        QVERIFY(!missing.success);
        QCOMPARE(missing.scheme.id, QStringLiteral("builtin-dark"));
        QVERIFY(!reopened.remove("builtin-dark"));
        QVERIFY(!reopened.remove("../escape"));
        QVERIFY(!reopened.add(builtinColorScheme(SchemeKind::Light)).success);
    }
    void catalogFailureAndLocking() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ColorSchemeCatalog catalog(dir.path());
        auto scheme = ColorSchemeReader::read(sparse()).scheme;
        QLockFile lock(dir.filePath(".catalog.lock"));
        QVERIFY(lock.tryLock());
        QVERIFY(!catalog.add(scheme).success);
        QVERIFY(!catalog.remove(scheme.id));
        lock.unlock();
        QVERIFY(catalog.add(scheme).success);
        QFile file(dir.filePath("test-scheme.ketscheme.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("broken");
        file.close();
        QVERIFY(!catalog.load(scheme.id).success);
        QCOMPARE(catalog.list().size(), 2);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(ColorSchemeWriter::write(builtinColorScheme(SchemeKind::Light)));
        file.close();
        QVERIFY(!catalog.load(scheme.id).success);
        QCOMPARE(catalog.list().size(), 2);
        ColorSchemeCatalog badRoot(file.fileName() + "/child");
        QVERIFY(!badRoot.add(scheme).success);
        scheme.id = "../escape";
        QVERIFY(!catalog.add(scheme).success);
    }
    void invalidTypedColorsAreNotPublished() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ColorSchemeCatalog catalog(dir.path());
        auto scheme = ColorSchemeReader::read(sparse()).scheme;
        scheme.editor["bg"] = QColor();
        const auto serialized = ColorSchemeReader::read(ColorSchemeWriter::write(scheme));
        QVERIFY(!serialized.ok());
        QVERIFY(!catalog.add(scheme).success);
        scheme.editor.clear();
        scheme.ui["malformed"] = QColor(1, 2, 3);
        QVERIFY(!catalog.add(scheme).success);
        QCOMPARE(catalog.list().size(), 2);
    }
    void boundsAndSymlinks() {
        QVERIFY(
            !ColorSchemeReader::read(QByteArray(ColorSchemeReader::maximumBytes + 1, ' ')).ok());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ColorSchemeCatalog catalog(dir.path());
        auto scheme = ColorSchemeReader::read(sparse()).scheme;
        const auto added = catalog.add(scheme);
        QVERIFY(added.success);
        const auto original = dir.filePath("test-scheme.ketscheme.json");
        const auto link = dir.filePath("alias.ketscheme.json");
        if (QFile::link(original, link) && QFileInfo(link).isSymLink()) {
            QVERIFY(!catalog.load("alias").success);
            QCOMPARE(catalog.list().size(), 3);
            QVERIFY(!catalog.remove("alias"));
        }
    }
    void defaultDirectory() {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(ColorSchemeCatalog().directory().endsWith(QStringLiteral("/color-schemes")));
        QCOMPARE(ColorSchemeCatalog().directory(),
                 QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                     "/color-schemes");
    }
};
QTEST_MAIN(ColorSchemeTest)
#include "ColorSchemeTest.moc"
