#include "ui/InterfaceFont.h"
#include "agent/AgentWorkspaceView.h"
#include "app/AppearanceSettings.h"
#include "ui/InterfaceZoom.h"
#include "ui/Theme.h"
#include "ui/TypeScale.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QRawFont>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

class InterfaceFontTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void resolvedFamilyPrefersEmbeddedKetSans();
    void embeddedWeightsCoverVietnamese_data();
    void embeddedWeightsCoverVietnamese();
    void agentTokensShareUiResolverAndEditorMono();
    void typeScaleApplyAndLiteralScalingAreMediumInert();
    void rejectsRawFontSizeLiteralsOutsideTypeScale();
};

void InterfaceFontTest::initTestCase() {
    QCoreApplication::setOrganizationName(QStringLiteral("KetPlus"));
    QCoreApplication::setApplicationName(QStringLiteral("KetPlus"));
}

void InterfaceFontTest::resolvedFamilyPrefersEmbeddedKetSans() {
    const QString family = ketplus::InterfaceUiFont::resolvedFamily();
    QCOMPARE(family, QStringLiteral("KET Sans"));
    QCOMPARE(ketplus::InterfaceUiFont::settingsLabel(), QStringLiteral("KÉT Sans"));
    const QString sheet = ketplus::InterfaceUiFont::styleSheetFamilyList();
    QVERIFY(sheet.startsWith(QStringLiteral("\"KET Sans\",")));
    QVERIFY(sheet.contains(QStringLiteral("\"SF Pro Text\"")));
    QVERIFY(sheet.contains(QStringLiteral("sans-serif")));
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        QVERIFY(family != QStringLiteral("Sans Serif"));
        const QString label = ketplus::InterfaceUiFont::settingsLabel();
        QCOMPARE(label, QStringLiteral("KÉT Sans"));
    }
#endif
}

void InterfaceFontTest::embeddedWeightsCoverVietnamese_data() {
    QTest::addColumn<int>("weight");
    QTest::newRow("light") << int(QFont::Light);
    QTest::newRow("regular") << int(QFont::Normal);
    QTest::newRow("medium") << int(QFont::Medium);
    QTest::newRow("bold") << int(QFont::Bold);
}

void InterfaceFontTest::embeddedWeightsCoverVietnamese() {
    QFETCH(int, weight);
    QFont font(ketplus::InterfaceUiFont::resolvedFamily());
    font.setPixelSize(16);
    font.setWeight(QFont::Weight(weight));
    const auto raw = QRawFont::fromFont(font);
    QVERIFY(raw.isValid());
    QCOMPARE(raw.familyName(), QStringLiteral("KET Sans"));
    QCOMPARE(int(raw.weight()), weight);
    const auto sample =
        QStringLiteral("Két Việt — Cộng hoà, quyền sở hữu, dữ liệu, ĐƯỜNG ₫ a\u0306\u0301");
    for (const auto ch : sample.toUcs4())
        QVERIFY2(raw.supportsCharacter(ch), qPrintable(QString("Missing U+%1").arg(ch, 4, 16)));
    const auto advances = raw.advancesForGlyphIndexes(raw.glyphIndexesForString("0123456789"));
    QCOMPARE(advances.size(), 10);
    for (const auto& advance : advances)
        QCOMPARE(advance.x(), advances.first().x());
}

void InterfaceFontTest::agentTokensShareUiResolverAndEditorMono() {
    QTemporaryDir settingsDirectory;
    QVERIFY(settingsDirectory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings().clear();

    auto appearance = ketplus::AppearanceSettings::defaults();
    appearance.editor.fontFamily = QStringLiteral("Courier New");
    appearance.save();
    QCOMPARE(ketplus::AppearanceSettings::load().editor.fontFamily, QStringLiteral("Courier New"));

    // First AgentWorkspaceView in this process resolves the current monoFont.
    ketplus::AgentWorkspaceView view;
    ketplus::ThemePalette palette;
    palette.pageBackground = QStringLiteral("#1b1f24");
    view.applyTheme(palette, ketplus::InterfaceZoomMetrics::mediumPixels);
    QCOMPARE(view.tokens().value(QStringLiteral("uiFont")).toString(),
             ketplus::InterfaceUiFont::resolvedFamily());
    QCOMPARE(view.tokens().value(QStringLiteral("monoFont")).toString(),
             QStringLiteral("Courier New"));
    QCOMPARE(view.tokens().value(QStringLiteral("reading")).toInt(), 16);
    QCOMPARE(view.tokens().value(QStringLiteral("body")).toInt(), 14);
    QCOMPARE(view.tokens().value(QStringLiteral("micro")).toInt(), 12);
    QCOMPARE(view.tokens().value(QStringLiteral("title")).toInt(), 18);
    QCOMPARE(view.tokens().value(QStringLiteral("glyphSmall")).toInt(), 16);
    QVERIFY(!view.tokens().contains(QStringLiteral("px9")));

    view.applyTheme(palette, ketplus::InterfaceZoomMetrics::largePixels);
    QCOMPARE(view.tokens().value(QStringLiteral("reading")).toInt(), 18);
    QCOMPARE(view.tokens().value(QStringLiteral("body")).toInt(), 16);
    QCOMPARE(view.tokens().value(QStringLiteral("glyphSmall")).toInt(), 18);
}

void InterfaceFontTest::typeScaleApplyAndLiteralScalingAreMediumInert() {
    const QString templateSheet = QStringLiteral(
        "QLabel { font-size: {{body}}px; } QPushButton { font-size: {{reading}}px; } "
        "QToolButton { font-size: {{title}}px; }");
    const QString medium =
        ketplus::TypeScale::apply(templateSheet, ketplus::InterfaceZoomMetrics::mediumPixels);
    QCOMPARE(medium, QStringLiteral("QLabel { font-size: 12px; } QPushButton { font-size: 14px; } "
                                    "QToolButton { font-size: 16px; }"));

    const QString large =
        ketplus::TypeScale::apply(templateSheet, ketplus::InterfaceZoomMetrics::largePixels);
    QCOMPARE(large, QStringLiteral("QLabel { font-size: 14px; } QPushButton { font-size: 16px; } "
                                   "QToolButton { font-size: 18px; }"));

    // Input samples for scaleLiteralFontSizes — intentional raw literals.
    const QString literals =
        QStringLiteral("QLabel { font-size: 12px; } QPushButton { font-size:14px; }");
    QCOMPARE(ketplus::TypeScale::scaleLiteralFontSizes(literals,
                                                       ketplus::InterfaceZoomMetrics::mediumPixels),
             literals);
    QCOMPARE(ketplus::TypeScale::scaleLiteralFontSizes(literals,
                                                       ketplus::InterfaceZoomMetrics::largePixels),
             QStringLiteral("QLabel { font-size: 14px; } QPushButton { font-size:16px; }"));
}

void InterfaceFontTest::rejectsRawFontSizeLiteralsOutsideTypeScale() {
    // tests/InterfaceFontTest.cpp → repo root.
    const QDir testDir = QFileInfo(QStringLiteral(__FILE__)).absoluteDir();
    const QString repoRoot = QDir(testDir.absoluteFilePath(QStringLiteral(".."))).absolutePath();
    QVERIFY(QFileInfo(repoRoot + QStringLiteral("/apps/desktop/src/ui/TypeScale.h")).exists());

    // Fixtures that intentionally embed SVG/CSS font-size samples or assert scaled
    // vendor/browser sheets. Keep this list short and commented.
    const QSet<QString> allowlist{
        // SVG fixture with inline font-size in ChatMermaidTest.
        QStringLiteral("tests/ChatMermaidTest.cpp"),
        // Asserts Markdown preview / browser defaultStyleSheet font-size literals.
        QStringLiteral("tests/MainWindowSettingsTest.cpp"),
        // scaleLiteralFontSizes input samples and expected expanded QSS strings.
        QStringLiteral("tests/InterfaceFontTest.cpp"),
        // Asserts ActivityBar expanded sheet pixel values after TypeScale::apply.
        QStringLiteral("tests/ExplorerPanelTest.cpp"),
    };

    static const QRegularExpression qssLiteral(QStringLiteral(R"(font-size:\s*\d+px)"));
    static const QRegularExpression qmlLiteral(QStringLiteral(R"(font\.pixelSize:\s*\d+)"));

    const QStringList roots{QStringLiteral("apps"), QStringLiteral("modules"),
                            QStringLiteral("tests")};
    QStringList offenders;
    for (const QString& rootName : roots) {
        const QString rootPath = repoRoot + QLatin1Char('/') + rootName;
        QDirIterator it(rootPath,
                        QStringList{QStringLiteral("*.cpp"), QStringLiteral("*.h"),
                                    QStringLiteral("*.qml"), QStringLiteral("*.qss")},
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString absolute = it.next();
            const QString relative = QDir(repoRoot).relativeFilePath(absolute);
            if (relative == QStringLiteral("apps/desktop/src/ui/TypeScale.h"))
                continue;
            if (relative.contains(QStringLiteral("/vendor/")) ||
                relative.contains(QStringLiteral("/build")) ||
                relative.contains(QStringLiteral("/.cache/")))
                continue;
            if (allowlist.contains(relative))
                continue;

            QFile file(absolute);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            const QString text = QString::fromUtf8(file.readAll());
            if (qssLiteral.match(text).hasMatch() || qmlLiteral.match(text).hasMatch())
                offenders.append(relative);
        }
    }

    if (!offenders.isEmpty()) {
        QFAIL(qPrintable(QStringLiteral("Raw font-size literals outside TypeScale:\n  ") +
                         offenders.join(QStringLiteral("\n  "))));
    }
}

QTEST_MAIN(InterfaceFontTest)
#include "InterfaceFontTest.moc"
