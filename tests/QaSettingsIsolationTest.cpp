#include "app/QaSettingsIsolation.h"
#include "update/AppUpdater.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

namespace {

constexpr auto kDirectoryVariable = "KETPLUS_QA_SETTINGS_DIR";

void useDirectory(const QString& path) { qputenv(kDirectoryVariable, QFile::encodeName(path)); }

} // namespace

class QaSettingsIsolationTest : public QObject {
    Q_OBJECT

  private slots:
    void init() { qunsetenv(kDirectoryVariable); }
    void cleanup() { qunsetenv(kDirectoryVariable); }
    void unsetKeepsNativeDefaults();
    void rejectsUnusablePaths();
    void isolatesNativeSettingsAndIniFallbacks();
};

void QaSettingsIsolationTest::unsetKeepsNativeDefaults() {
    QCOMPARE(QSettings::defaultFormat(), QSettings::NativeFormat);
    QVERIFY(ketplus::configureQaSettingsIsolation(QStringLiteral("qa"), QStringLiteral("qa"))
                .isEmpty());
    QCOMPARE(QSettings::defaultFormat(), QSettings::NativeFormat);
}

void QaSettingsIsolationTest::rejectsUnusablePaths() {
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const auto organization = QStringLiteral("qa");
    const auto application = QStringLiteral("desktop");
    const auto expectError = [&] {
        const auto error = ketplus::configureQaSettingsIsolation(organization, application);
        QVERIFY2(error.startsWith(QStringLiteral("KETPLUS_QA_SETTINGS_DIR")), qPrintable(error));
        QCOMPARE(QSettings::defaultFormat(), QSettings::NativeFormat);
    };

#ifndef Q_OS_WIN
    // qputenv removes the variable when given an empty value on Windows.
    useDirectory(QString{});
    expectError();
#endif
    useDirectory(QStringLiteral("relative"));
    expectError();

    QFile blockingFile(scratch.filePath(QStringLiteral("file")));
    QVERIFY(blockingFile.open(QIODevice::WriteOnly));
    blockingFile.close();
    useDirectory(blockingFile.fileName());
    expectError();

    useDirectory(scratch.path());
    QVERIFY(QDir().mkpath(scratch.filePath(QStringLiteral("user/qa/desktop.ini"))));
    expectError();

#ifdef Q_OS_UNIX
    const auto linkedRoot = scratch.filePath(QStringLiteral("linked"));
    QVERIFY(QDir().mkpath(linkedRoot));
    QVERIFY(QFile::link(scratch.path(), QDir(linkedRoot).filePath(QStringLiteral("system"))));
    useDirectory(linkedRoot);
    expectError();
#endif
}

void QaSettingsIsolationTest::isolatesNativeSettingsAndIniFallbacks() {
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const auto organization = QStringLiteral("dev.ketplus.tests.qa.") +
                              QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto application = QStringLiteral("SettingsProbe");
    QCoreApplication::setOrganizationName(organization);
    QCoreApplication::setApplicationName(application);

    const auto lastRoot = QStringLiteral("workspace/lastRoot");
    const auto draft = QStringLiteral("agent/drafts/qa");
    const auto recent = QStringLiteral("workspace/recentFolders");
    const auto repositories = QStringLiteral("spotlight/repositories");
    const auto sessions = QStringLiteral("agent/sessions/qa");
    const auto nativeValue = QStringLiteral("native-only");
    const auto isolatedValue = QStringLiteral("isolated-only");

    // Use a unique native domain so the regression can read, write and compare actual
    // native values without ever opening the installed app's domain.
    QSettings native(QSettings::NativeFormat, QSettings::UserScope, organization, application);
    native.setFallbacksEnabled(false);
    const auto cleanupNative = qScopeGuard([&] {
        native.clear();
        native.sync();
    });
    native.setValue(lastRoot, nativeValue);
    native.setValue(sessions, nativeValue);
    native.sync();
    QCOMPARE(native.status(), QSettings::NoError);
    QCOMPARE(QSettings().value(lastRoot).toString(), nativeValue);

    // Seed the ordinary INI organization and system fallback locations separately.
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       scratch.filePath(QStringLiteral("ordinary-user")));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                       scratch.filePath(QStringLiteral("ordinary-system")));
    QSettings userFallback(QSettings::IniFormat, QSettings::UserScope, organization);
    userFallback.setValue(recent, nativeValue);
    userFallback.sync();
    QCOMPARE(userFallback.status(), QSettings::NoError);
    QSettings systemFallback(QSettings::IniFormat, QSettings::SystemScope, organization,
                             application);
    systemFallback.setValue(repositories, nativeValue);
    systemFallback.sync();
    QCOMPARE(systemFallback.status(), QSettings::NoError);
    QSettings ordinary(QSettings::IniFormat, QSettings::UserScope, organization, application);
    QCOMPARE(ordinary.value(recent).toString(), nativeValue);
    QCOMPARE(ordinary.value(repositories).toString(), nativeValue);

    const auto isolatedPath = scratch.filePath(QStringLiteral("isolated"));
    useDirectory(isolatedPath);
    const auto error = ketplus::configureQaSettingsIsolation(organization, application);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(QSettings::defaultFormat(), QSettings::IniFormat);
    for (const auto& key : {lastRoot, recent, repositories, sessions, draft})
        QVERIFY2(!QSettings().contains(key), qPrintable(key));

    {
        QSettings isolated;
        for (const auto& key : {lastRoot, recent, repositories, sessions, draft})
            isolated.setValue(key, isolatedValue);
        isolated.sync();
        QCOMPARE(isolated.status(), QSettings::NoError);
    }
    const auto relativeFile = QStringLiteral("user/%1/%2.ini").arg(organization, application);
    QSettings persisted(QDir(isolatedPath).filePath(relativeFile), QSettings::IniFormat);
    for (const auto& key : {lastRoot, recent, repositories, sessions, draft}) {
        QCOMPARE(QSettings().value(key).toString(), isolatedValue);
        QCOMPARE(persisted.value(key).toString(), isolatedValue);
    }

    native.sync();
    QCOMPARE(native.value(lastRoot).toString(), nativeValue);
    QCOMPARE(native.value(sessions).toString(), nativeValue);
    QVERIFY(!native.contains(draft));
    QVERIFY(!native.contains(recent));
    QVERIFY(!native.contains(repositories));
    userFallback.sync();
    systemFallback.sync();
    QCOMPARE(userFallback.value(recent).toString(), nativeValue);
    QCOMPARE(systemFallback.value(repositories).toString(), nativeValue);

    auto& updater = ketplus::AppUpdater::instance();
    updater.start();
    updater.checkForUpdates();
    QVERIFY(!updater.isConfigured());
    QVERIFY(!updater.canCheckForUpdates());
}

QTEST_GUILESS_MAIN(QaSettingsIsolationTest)

#include "QaSettingsIsolationTest.moc"
