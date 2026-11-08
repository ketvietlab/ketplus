#include "ui/InterfaceFont.h"
#include "KetAppearance.h"
#include "KetTokens.h"
#include "agent/AgentWorkspaceView.h"
#include "app/AppearanceSettings.h"
#include "ui/InterfaceZoom.h"
#include "ui/Theme.h"
#include "ui/TypeScale.h"
#include <QQuickItem>
#include <QQuickView>

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQuickTextDocument>
#include <QRawFont>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextTable>
#include <QtTest>

class InterfaceFontTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    // Run first: another font test must not preload fonts and hide a startup failure.
    void agentThemeUsesKitAndEditorMono();
    void resolvedFamilyPrefersEmbeddedKetSans();
    void embeddedWeightsCoverVietnamese_data();
    void embeddedWeightsCoverVietnamese();
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
    QVERIFY(!sheet.contains(QStringLiteral("SF Pro")));
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

void InterfaceFontTest::agentThemeUsesKitAndEditorMono() {
    QTemporaryDir settingsDirectory;
    QVERIFY(settingsDirectory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings().clear();

    auto appearance = ketplus::AppearanceSettings::defaults();
    appearance.editor.fontFamily = QStringLiteral("Courier New");
    appearance.save();
    QCOMPARE(ketplus::AppearanceSettings::load().editor.fontFamily, QStringLiteral("Courier New"));

    ketplus::ThemeManager theme(*qApp);
    auto& kit = ket::Appearance::instance();
    const auto previousMode = kit.mode();
    const auto previousZoom = kit.zoom();
    const auto original = ket::Profile::current();
    const auto restore = qScopeGuard([&] {
        ket::Profile::setCurrent(original);
        kit.setMode(previousMode);
        kit.setZoom(previousZoom);
    });
    QTest::failOnWarning(QRegularExpression(".*(Unable to assign|ReferenceError|TypeError).*"));
    ketplus::AgentWorkspaceView view;
    view.resize(1100, 740);
    view.setAgentAvailable(true);
    view.setSessionState("Review theme and typography", "Ready", false);
    view.resetMessages(
        {QVariantMap{{"kind", "user"},
                     {"messageKey", "theme-prompt"},
                     {"body", "Keep the conversation readable as I change theme and zoom."}},
         QVariantMap{{"kind", "assistant"},
                     {"messageKey", "theme-answer"},
                     {"body", "## Shared appearance\n\nThe header, `inline code` and transcript "
                              "follow the same theme. Your editor font stays selected.\n\n"
                              "```cpp\nconst auto zoom = appearance.zoom();\n```\n\n"
                              "| Surface | State |\n| --- | --- |\n| Conversation | Ready |"}}},
        "theme-authority");
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* title = root->findChild<QQuickItem*>("conversationTitle");
    auto* number = root->findChild<QQuickItem*>("conversationNumber");
    auto* pane = root->findChild<QQuickItem*>("chatPane");
    QVERIFY(title && number && pane);
    QCOMPARE(view.monoFontFamily(), QStringLiteral("Courier New"));
    QCOMPARE(number->property("font").value<QFont>().family(), QStringLiteral("Courier New"));
    QCOMPARE(title->property("font").value<QFont>().family(),
             ketplus::InterfaceUiFont::resolvedFamily());
    QVERIFY(view.metaObject()->indexOfProperty("tokens") < 0);

    const auto findVisual = [](auto&& self, QQuickItem* item, const QString& name) -> QQuickItem* {
        if (item->objectName() == name)
            return item;
        for (auto* child : item->childItems())
            if (auto* found = self(self, child, name))
                return found;
        return nullptr;
    };
    const auto documentGeometryMatches = [&] {
        auto* frame = findVisual(findVisual, root, "messageCodeBlockFrame");
        if (!frame)
            return false;
        auto* body = frame->parentItem();
        auto* quickDoc = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
        if (!quickDoc)
            return false;
        auto* doc = quickDoc->textDocument();
        const auto code = doc->find("const auto zoom");
        QRectF caret;
        if (code.isNull() ||
            !QMetaObject::invokeMethod(body, "positionToRectangle", Q_RETURN_ARG(QRectF, caret),
                                       Q_ARG(int, code.selectionStart())))
            return false;
        if (!QRectF(frame->x(), frame->y(), frame->width(), frame->height())
                 .contains(caret.center()))
            return false;
        for (auto* child : doc->rootFrame()->childFrames()) {
            auto* table = qobject_cast<QTextTable*>(child);
            if (!table)
                continue;
            const auto first = table->cellAt(0, 0).firstCursorPosition().block();
            const auto second = table->cellAt(0, 1).firstCursorPosition().block();
            const auto a = doc->documentLayout()->blockBoundingRect(first);
            const auto b = doc->documentLayout()->blockBoundingRect(second);
            return first.text() == "Surface" && qAbs(a.y() - b.y()) < 1 &&
                   a.y() >= frame->y() + frame->height() - body->property("topPadding").toReal();
        }
        return false;
    };
    // No host applyTheme call: every mounted view must react to the shared authority.
    for (bool dark : {false, true}) {
        kit.setMode(dark ? ket::Appearance::Mode::Dark : ket::Appearance::Mode::Light);
        for (qreal zoom : {1.0, 1.25, 1.0}) {
            kit.setZoom(zoom);
            QTRY_COMPARE(title->property("font").value<QFont>().pixelSize(),
                         qRound(ket::Appearance::scale(original.readingPixels, zoom)));
            QTRY_COMPARE(number->property("font").value<QFont>().pixelSize(),
                         qRound(ket::Appearance::scale(original.buttonFontPixels, zoom)));
            QTRY_COMPARE(title->property("color").value<QColor>(),
                         ket::Tokens::color(u"text-main", kit.theme()));
            QTRY_COMPARE(pane->property("color").value<QColor>(),
                         ket::Tokens::color(u"page-bg", kit.theme()));
            QCOMPARE(view.quickView()->color(), ket::Tokens::color(u"page-bg", kit.theme()));
            QTRY_VERIFY(documentGeometryMatches());
            auto* logo = findVisual(findVisual, root, "productBrandImage");
            QVERIFY(logo);
            QVERIFY(logo->x() >= 0 && logo->x() + logo->width() <= logo->parentItem()->width());
            const auto capture = qEnvironmentVariable("KETPLUS_THEME_AUTHORITY_CAPTURE");
            if (!capture.isEmpty()) {
                QTest::qWait(60);
                QVERIFY(view.quickView()->grabWindow().save(
                    capture +
                    QString("-%1-%2.png").arg(dark ? "dark" : "light").arg(qRound(zoom * 100))));
            }
        }
    }
    auto custom = original;
    custom.readingPixels = 19;
    ket::Profile::setCurrent(custom);
    kit.setZoom(18.0 / 14);
    QTRY_COMPARE(title->property("font").value<QFont>().pixelSize(), qRound(19 * 18.0 / 14));
    appearance.editor.fontFamily = QStringLiteral("KÉT Sans Mono");
    appearance.save();
    view.applyTheme(ketplus::currentThemePalette());
    QTRY_COMPARE(number->property("font").value<QFont>().family(),
                 ketplus::AppearanceSettings::load().editor.fontFamily);
}

void InterfaceFontTest::typeScaleApplyAndLiteralScalingAreMediumInert() {
    const QString templateSheet = QStringLiteral(
        "QLabel { font-size: {{body}}px; } QPushButton { font-size: {{reading}}px; } "
        "QToolButton { font-size: {{title}}px; }");
    const QString medium =
        ketplus::TypeScale::apply(templateSheet, ketplus::InterfaceZoomMetrics::mediumPixels);
    const auto& p = ket::Profile::current();
    const auto expected = [&p](int pixels) {
        const auto px = [pixels](qreal n) {
            return ketplus::InterfaceZoomMetrics::scale(qRound(n), pixels);
        };
        return QStringLiteral("QLabel { font-size: %1px; } QPushButton { font-size: %2px; } "
                              "QToolButton { font-size: %3px; }")
            .arg(px(p.buttonFontPixels))
            .arg(px(p.readingPixels))
            .arg(px(p.dialogTitlePixels));
    };
    QCOMPARE(medium, expected(14));
    QCOMPARE(ketplus::TypeScale::apply(templateSheet, 16), expected(16));

    QCOMPARE(ketplus::TypeScale::apply(QStringLiteral("QLabel { font-size:{{px:30}}px; }"), 21),
             QStringLiteral("QLabel { font-size:45px; }"));
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
