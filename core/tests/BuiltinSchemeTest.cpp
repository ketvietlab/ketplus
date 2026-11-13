// SPDX-License-Identifier: MPL-2.0
#include "ui/theme/ColorSchemeCatalog.h"
#include "ui/theme/ColorSchemeTokens.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

using namespace ketplus;
namespace {
double luminance(const QColor& color) {
    const auto linear = [](double channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
           0.0722 * linear(color.blueF());
}
} // namespace
class BuiltinSchemeTest : public QObject {
    Q_OBJECT
  private slots:
    void catalogOrderAndDefaults() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto schemes = ColorSchemeCatalog(directory.path()).list();
        QCOMPARE(schemes.size(), 10);
        QCOMPARE(schemes[0].id, QStringLiteral("builtin-light"));
        QCOMPARE(schemes[1].id, QStringLiteral("builtin-dark"));
        QVERIFY(schemes[0] == builtinColorScheme(SchemeKind::Light));
        QVERIFY(schemes[1] == builtinColorScheme(SchemeKind::Dark));
        QSet<QString> ids;
        for (const auto& scheme : schemes)
            ids.insert(scheme.id);
        QCOMPARE(ids.size(), 10);
    }
    void builtins_data() {
        QTest::addColumn<QString>("id");
        QTest::addColumn<bool>("light");
        QTest::addColumn<QString>("foreground");
        QTest::addColumn<QString>("background");
        QTest::addColumn<QString>("source");
        QTest::newRow("ket-light")
            << QStringLiteral("builtin-light") << true << QStringLiteral("#24262A")
            << QStringLiteral("#FFFFFF") << QStringLiteral("");
        QTest::newRow("ket-dark") << QStringLiteral("builtin-dark") << false
                                  << QStringLiteral("#F2F4F7") << QStringLiteral("#1D2228")
                                  << QStringLiteral("");
        QTest::newRow("dark-plus")
            << QStringLiteral("builtin.vscode-dark-plus") << false << QStringLiteral("#D4D4D4")
            << QStringLiteral("#1E1E1E") << QStringLiteral("VS Code");
        QTest::newRow("light-plus")
            << QStringLiteral("builtin.vscode-light-plus") << true << QStringLiteral("#000000")
            << QStringLiteral("#FFFFFF") << QStringLiteral("VS Code");
        QTest::newRow("darcula") << QStringLiteral("builtin.darcula") << false
                                 << QStringLiteral("#A9B7C6") << QStringLiteral("#2B2B2B")
                                 << QStringLiteral("JetBrains");
        QTest::newRow("intellij-light")
            << QStringLiteral("builtin.intellij-light") << true << QStringLiteral("#080808")
            << QStringLiteral("#FFFFFF") << QStringLiteral("JetBrains");
        QTest::newRow("dracula") << QStringLiteral("builtin.dracula") << false
                                 << QStringLiteral("#F8F8F2") << QStringLiteral("#282A36")
                                 << QStringLiteral("Community");
        QTest::newRow("one-dark-pro")
            << QStringLiteral("builtin.one-dark-pro") << false << QStringLiteral("#ABB2BF")
            << QStringLiteral("#282C34") << QStringLiteral("Community");
        QTest::newRow("nord") << QStringLiteral("builtin.nord") << false
                              << QStringLiteral("#D8DEE9") << QStringLiteral("#2E3440")
                              << QStringLiteral("Community");
        QTest::newRow("solarized-dark")
            << QStringLiteral("builtin.solarized-dark") << false << QStringLiteral("#839496")
            << QStringLiteral("#002B36") << QStringLiteral("VS Code");
    }
    void builtins() {
        QFETCH(QString, id);
        QFETCH(bool, light);
        QFETCH(QString, foreground);
        QFETCH(QString, background);
        QFETCH(QString, source);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ColorSchemeCatalog catalog(directory.path());
        const auto loaded = catalog.load(id);
        QVERIFY(loaded.success);
        QVERIFY(loaded.diagnostics.isEmpty());
        QCOMPARE(loaded.scheme.kind, light ? SchemeKind::Light : SchemeKind::Dark);
        const auto roundTrip = ColorSchemeReader::read(ColorSchemeWriter::write(loaded.scheme));
        QVERIFY(roundTrip.ok());
        QVERIFY(roundTrip.scheme == loaded.scheme);
        const auto resolved = resolveColorScheme(loaded.scheme);
        for (const auto& group : {std::pair{&resolved.ui, &scheme_detail::uiKeys},
                                  std::pair{&resolved.editor, &scheme_detail::editorKeys},
                                  std::pair{&resolved.terminal, &scheme_detail::terminalKeys},
                                  std::pair{&resolved.diff, &scheme_detail::diffKeys}})
            for (const auto& key : *group.second)
                QVERIFY2(group.first->value(key).isValid(), qPrintable(id + ':' + key));
        for (const auto& key : scheme_detail::syntaxKeys) {
            const auto style = resolved.syntax.value(key);
            QVERIFY(style.fg && style.fg->isValid());
            QVERIFY(style.bold.has_value() && style.italic.has_value() &&
                    style.underline.has_value());
        }
        const auto fg = resolved.editor.value("fg"), bg = resolved.editor.value("bg");
        QCOMPARE(fg, QColor(foreground));
        QCOMPARE(bg, QColor(background));
        QCOMPARE(fg.alpha(), 255);
        QCOMPARE(bg.alpha(), 255);
        const double a = luminance(fg), b = luminance(bg);
        const double contrast = (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
        qInfo().noquote() << id << "editor contrast" << contrast;
        // Preserve upstream colors if a future approved snapshot falls below WCAG AA.
        // Such exceptions require a documented provenance warning, never recoloring.
        if (contrast < 4.5) {
            qWarning().noquote() << id << "upstream editor contrast below 4.5:1";
            QVERIFY(!loaded.scheme.metadata.value("contrastWarning").toString().isEmpty());
        }
        QVERIFY(!catalog.add(loaded.scheme).success);
        QVERIFY(!catalog.remove(id));
        const auto duplicate = catalog.duplicate(id, QStringLiteral("My copy"));
        QVERIFY(duplicate.success);
        QVERIFY(!duplicate.scheme.id.startsWith(u"builtin"));
        QCOMPARE(duplicate.scheme.editor, loaded.scheme.editor);
        QVERIFY(catalog.remove(duplicate.scheme.id));
        if (source.isEmpty())
            return;
        const auto metadata = loaded.scheme.metadata;
        QCOMPARE(metadata.value("sourceLabel").toString(), source);
        QVERIFY(!metadata.value("author").toString().isEmpty());
        if (id == u"builtin.vscode-dark-plus" || id == u"builtin.vscode-light-plus") {
            QCOMPARE(metadata["author"].toString(), QStringLiteral("Microsoft Corporation"));
            QVERIFY(!metadata.contains("upstreamCredits"));
        }
        if (id == u"builtin.solarized-dark") {
            QCOMPARE(metadata["author"].toString(), QStringLiteral("Microsoft Corporation"));
            const auto credits = metadata["upstreamCredits"].toArray();
            QCOMPARE(credits.size(), 1);
            QCOMPARE(credits[0].toObject()["author"].toString(),
                     QStringLiteral("Ethan Schoonover"));
            QCOMPARE(credits[0].toObject()["repo"].toString(),
                     QStringLiteral("altercation/solarized"));
        }
        QVERIFY(metadata["license"] == "MIT" || metadata["license"] == "Apache-2.0");
        QVERIFY(metadata["manualAdjustments"].isArray());
        const auto upstream = metadata["upstream"].toObject();
        QVERIFY(
            QRegularExpression("^[0-9a-f]{40}$").match(upstream["commit"].toString()).hasMatch());
        QVERIFY(!upstream["files"].toArray().isEmpty());
        for (const auto& entry : upstream["files"].toArray())
            QVERIFY(QRegularExpression("^[0-9a-f]{64}$")
                        .match(entry.toObject()["sha256"].toString())
                        .hasMatch());
        const auto reportMetadata = metadata["importReport"].toObject();
        QFile reportFile(QStringLiteral(KETPLUS_SOURCE_ROOT "/") +
                         reportMetadata["path"].toString());
        QVERIFY(reportFile.open(QIODevice::ReadOnly));
        const auto report = QJsonDocument::fromJson(reportFile.readAll()).object();
        QCOMPARE(report["counts"], reportMetadata["counts"]);
        QJsonObject actual{{"mapped", 0}, {"approximated", 0}, {"skipped", 0}};
        for (const auto& item : report["items"].toArray()) {
            const auto object = item.toObject();
            const auto disposition = object["disposition"].toString();
            QVERIFY(actual.contains(disposition));
            actual[disposition] = actual[disposition].toInt() + 1;
            QVERIFY(!object["reason"].toString().isEmpty());
        }
        QCOMPARE(actual, report["counts"].toObject());
    }
    void reservedNamespaceCannotBeShadowed() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ColorSchemeCatalog catalog(directory.path());
        auto impostor = builtinColorScheme(SchemeKind::Light);
        for (const auto* id : {"builtin.dracula", "builtin.future-theme", "builtin-legacy"}) {
            impostor.id = QString::fromLatin1(id);
            QVERIFY(!catalog.add(impostor).success);
            QVERIFY(!catalog.remove(impostor.id));
        }
        impostor.id = QStringLiteral("builtin.dracula");
        QFile collision(directory.filePath("builtin.dracula.ketscheme.json"));
        QVERIFY(collision.open(QIODevice::WriteOnly));
        collision.write(ColorSchemeWriter::write(impostor));
        collision.close();
        QCOMPARE(catalog.list().size(), 10);
        QCOMPARE(catalog.load(impostor.id).scheme.name, QStringLiteral("Dracula"));
        QVERIFY(!catalog.load(QStringLiteral("builtin.future-theme")).success);
    }
};
QTEST_MAIN(BuiltinSchemeTest)
#include "BuiltinSchemeTest.moc"
