#include "KetAppearance.h"
#include "KetTokens.h"
#include "ui/Theme.h"
#include "ui/theme/ColorScheme.h"
#include "ui/theme/ColorSchemeCatalog.h"
#include "ui/theme/ColorSchemeKitAdapter.h"
#include "ui/theme/ThemeSelection.h"
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>
using namespace ketplus;
namespace {
ThemeSelection fixed(const QString& id) {
    ThemeSelection value;
    value.followSystem = false;
    value.schemeId = id;
    return value;
}
ColorScheme custom(const QString& id) {
    auto value = builtinColorScheme(SchemeKind::Dark);
    value.id = id;
    value.name = id;
    value.ui[QStringLiteral("chrome.panelBackground")] = QColor(12, 23, 34);
    value.ui[QStringLiteral("chrome.accent")] = QColor(100, 110, 120);
    value.syntax[QStringLiteral("keyword")].fg = QColor(200, 210, 220);
    return value;
}
} // namespace
class SchemeManagerTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("KetPlusTests"));
        QCoreApplication::setApplicationName(QStringLiteral("SchemeManager"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    }
    void cleanup() {
        QThreadPool::globalInstance()->waitForDone();
        ket::Appearance::instance().clearColorOverrides(ket::Theme::Light);
        ket::Appearance::instance().clearColorOverrides(ket::Theme::Dark);
        QSettings().clear();
    }
    void builtinKitDefaultsStayUntouched() {
        for (const auto kind : {SchemeKind::Light, SchemeKind::Dark})
            QVERIFY(colorSchemeKitOverrides(builtinColorScheme(kind)).isEmpty());
        const auto scheme = custom(QStringLiteral("custom"));
        const auto overrides = colorSchemeKitOverrides(scheme);
        QCOMPARE(overrides.value(QStringLiteral("panel-bg")), QColor(12, 23, 34));
        QCOMPARE(overrides.value(QStringLiteral("accent")), QColor(100, 110, 120));
        QVERIFY(!overrides.contains(QStringLiteral("chart-1")));
        auto& appearance = ket::Appearance::instance();
        QVERIFY(appearance.setColorOverrides(ket::Theme::Dark, overrides).isEmpty());
        QCOMPARE(ket::Tokens::color(u"panel-bg", ket::Theme::Dark), QColor(12, 23, 34));
    }
    void builtinPaletteMatchesUnmodifiedKit() {
        QTemporaryDir directory;
        ThemeManager manager(*qApp, nullptr, directory.path());
        for (const auto mode : {ThemeManager::Mode::Light, ThemeManager::Mode::Dark}) {
            manager.setMode(mode);
            QVERIFY(manager.palette() == currentThemePalette());
        }
    }
    void migrationAndCommittedMode() {
        QTemporaryDir directory;
        QSettings().setValue(QStringLiteral("appearance/theme"), QStringLiteral("dark"));
        {
            ThemeManager manager(*qApp, nullptr, directory.path());
            QCOMPARE(manager.mode(), ThemeManager::Mode::Dark);
            QCOMPARE(manager.colorScheme().id, QStringLiteral("builtin-dark"));
        }
        QSettings().setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
        ThemeManager manager(*qApp, nullptr, directory.path());
        QCOMPARE(manager.mode(), ThemeManager::Mode::Dark);
        QSignalSpy changed(&manager, &ThemeManager::themeChanged);
        QSignalSpy kitChanged(&ket::Appearance::instance(), &ket::Appearance::themeChanged);
        manager.setMode(ThemeManager::Mode::Light);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(kitChanged.size(), 1);
        QCOMPARE(manager.colorScheme().id, QStringLiteral("builtin-light"));
        manager.setMode(ThemeManager::Mode::Light);
        QCOMPARE(changed.size(), 1);
    }
    void previewCancelAndCommit() {
        QTemporaryDir directory;
        ThemeManager manager(*qApp, nullptr, directory.path());
        manager.setMode(ThemeManager::Mode::Light);
        ThemeSelectionStore observer(directory.path());
        const auto revision = observer.revision();
        QSignalSpy changed(&manager, &ThemeManager::themeChanged);
        manager.previewMode(ThemeManager::Mode::Dark);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(manager.mode(), ThemeManager::Mode::Light);
        QVERIFY(manager.palette().dark);
        observer.refresh();
        QCOMPARE(observer.revision(), revision);
        manager.cancelPreview();
        QCOMPARE(changed.size(), 2);
        QVERIFY(!manager.palette().dark);
        manager.previewMode(ThemeManager::Mode::Dark);
        QVERIFY(manager.commitSchemePreview());
        QCOMPARE(manager.mode(), ThemeManager::Mode::Dark);
        QVERIFY(!manager.hasSchemePreview());
        observer.refresh();
        QCOMPARE(observer.selection().schemeId, QStringLiteral("builtin-dark"));
        QVERIFY(observer.revision() > revision);
    }
    void rejectedCommitKeepsPreviewAndCommittedSelection() {
        QTemporaryDir directory;
        ThemeManager manager(*qApp, nullptr, directory.path());
        manager.setMode(ThemeManager::Mode::Light);
        manager.previewMode(ThemeManager::Mode::Dark);
        QLockFile lock(QDir(directory.path()).filePath(QStringLiteral(".selection.lock")));
        QVERIFY(lock.tryLock(0));
        QSignalSpy changed(&manager, &ThemeManager::themeChanged);
        QSignalSpy failed(&manager, &ThemeManager::schemeFailed);
        QVERIFY(!manager.commitSchemePreview());
        QCOMPARE(changed.size(), 0);
        QCOMPARE(failed.size(), 1);
        QVERIFY(!manager.schemeError().isEmpty());
        QCOMPARE(manager.mode(), ThemeManager::Mode::Light);
        QVERIFY(manager.hasSchemePreview());
        QVERIFY(manager.palette().dark);
    }
    void installedSchemePublishesCompleteSnapshotOnce() {
        QTemporaryDir directory;
        QVERIFY(ColorSchemeCatalog(directory.path()).add(custom(QStringLiteral("custom"))).success);
        ThemeManager manager(*qApp, nullptr, directory.path());
        QSignalSpy changed(&manager, &ThemeManager::themeChanged);
        bool complete = false;
        connect(&manager, &ThemeManager::themeChanged, this, [&] {
            complete = manager.colorScheme().id == u"custom" && manager.palette().dark &&
                       ket::Tokens::color(u"panel-bg", ket::Theme::Dark) == QColor(12, 23, 34);
        });
        QVERIFY(manager.setSchemeSelection(fixed(QStringLiteral("custom"))));
        QTRY_COMPARE(changed.size(), 1);
        QVERIFY(complete);
        QCOMPARE(manager.colorScheme().syntax.value(QStringLiteral("keyword")).fg,
                 std::optional<QColor>(QColor(200, 210, 220)));
    }
    void supersededPreviewCannotPublish() {
        QTemporaryDir directory;
        ThemeManager manager(*qApp, nullptr, directory.path());
        manager.setMode(ThemeManager::Mode::Light);
        QSignalSpy changed(&manager, &ThemeManager::themeChanged);
        manager.previewColorScheme(custom(QStringLiteral("preview")));
        manager.cancelPreview();
        QCOMPARE(changed.size(), 1);
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QCOMPARE(changed.size(), 1);
        QCOMPARE(manager.colorScheme().id, QStringLiteral("builtin-light"));
    }
    void setModeRecoversCorruptSelection() {
        QTemporaryDir directory;
        ThemeManager manager(*qApp, nullptr, directory.path());
        manager.setMode(ThemeManager::Mode::Light);
        QFile file(directory.filePath("selection.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("broken");
        file.close();
        QSignalSpy changed(&manager, &ThemeManager::themeChanged);
        manager.setMode(ThemeManager::Mode::Dark);
        QCOMPARE(manager.mode(), ThemeManager::Mode::Dark);
        QCOMPARE(manager.colorScheme().id, QStringLiteral("builtin-dark"));
        QVERIFY(manager.schemeError().isEmpty());
        QCOMPARE(changed.size(), 1);
        ThemeSelectionStore reopened(directory.path());
        QVERIFY(reopened.lastError().isEmpty());
        QCOMPARE(reopened.selection(), manager.schemeSelection());
        manager.setMode(ThemeManager::Mode::Light);
        QCOMPARE(manager.colorScheme().id, QStringLiteral("builtin-light"));
        QCOMPARE(changed.size(), 2);
    }
    void externalCommitCancelsLocalPreview() {
        QTemporaryDir directory;
        ThemeManager manager(*qApp, nullptr, directory.path());
        manager.setMode(ThemeManager::Mode::Light);
        manager.previewMode(ThemeManager::Mode::Dark);
        ThemeSelectionStore otherProcess(directory.path());
        auto wanted = fixed(QStringLiteral("builtin-light"));
        wanted.darkSchemeId = QStringLiteral("missing-preferred-dark");
        QVERIFY(otherProcess.commit(wanted));
        QTRY_VERIFY(!manager.hasSchemePreview());
        QCOMPARE(manager.schemeSelection(), wanted);
        QCOMPARE(manager.colorScheme().id, QStringLiteral("builtin-light"));
    }
    void systemPreferenceAndMissingIdFallback() {
        QTemporaryDir directory;
        ThemeManager manager(*qApp, nullptr, directory.path());
        auto wanted = manager.schemeSelection();
        wanted.followSystem = true;
        // Equal preferences make the expected identity independent of the host OS.
        wanted.lightSchemeId = wanted.darkSchemeId = QStringLiteral("builtin-dark");
        QVERIFY(manager.setSchemeSelection(wanted));
        QCOMPARE(manager.mode(), ThemeManager::Mode::System);
        QCOMPARE(manager.colorScheme().id, QStringLiteral("builtin-dark"));
        QVERIFY(manager.setSchemeSelection(fixed(QStringLiteral("missing-id"))));
        QTRY_COMPARE(manager.colorScheme().id, QStringLiteral("builtin-light"));
        QCOMPARE(manager.schemeSelection().schemeId, QStringLiteral("missing-id"));
    }

  private:
    QTemporaryDir settings_;
};
QTEST_MAIN(SchemeManagerTest)
#include "SchemeManagerTest.moc"
