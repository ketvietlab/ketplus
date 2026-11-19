#include "app/QaSettingsIsolation.h"
#include "update/AppUpdater.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_MACOS
void observeQaNativeSettingsReads(int* count);
#endif

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
    const auto organization = QStringLiteral("dev.ketplus.tests.qa.isolation");
    const auto application = QStringLiteral("SettingsProbe");
    QCoreApplication::setOrganizationName(organization);
    QCoreApplication::setApplicationName(application);

    const auto lastRoot = QStringLiteral("workspace/lastRoot");
    const auto draft = QStringLiteral("agent/drafts/qa");
    const auto recent = QStringLiteral("workspace/recentFolders");
    const auto repositories = QStringLiteral("spotlight/repositories");
    const auto sessions = QStringLiteral("agent/sessions/qa");
    const auto ordinaryValue = QStringLiteral("ordinary-only");
    const auto isolatedValue = QStringLiteral("isolated-only");

#ifdef Q_OS_MACOS
    int readCount = 0;
    observeQaNativeSettingsReads(&readCount);
    const auto stopObserving = qScopeGuard([] { observeQaNativeSettingsReads(nullptr); });
    QCOMPARE(QSettings().value(lastRoot).toString(), QStringLiteral("native-sentinel"));
    QVERIFY(readCount > 0);
    readCount = 0;
#endif

    // Seed the ordinary INI organization and system fallback locations separately.
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       scratch.filePath(QStringLiteral("ordinary-user")));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                       scratch.filePath(QStringLiteral("ordinary-system")));
    QSettings userFallback(QSettings::IniFormat, QSettings::UserScope, organization);
    userFallback.setValue(recent, ordinaryValue);
    userFallback.sync();
    QCOMPARE(userFallback.status(), QSettings::NoError);
    QSettings systemFallback(QSettings::IniFormat, QSettings::SystemScope, organization,
                             application);
    systemFallback.setValue(repositories, ordinaryValue);
    systemFallback.sync();
    QCOMPARE(systemFallback.status(), QSettings::NoError);
    QSettings ordinary(QSettings::IniFormat, QSettings::UserScope, organization, application);
    QCOMPARE(ordinary.value(recent).toString(), ordinaryValue);
    QCOMPARE(ordinary.value(repositories).toString(), ordinaryValue);

    const auto isolatedPath = scratch.filePath(QStringLiteral("isolated"));
    useDirectory(isolatedPath);
    const auto error = ketplus::configureQaSettingsIsolation(organization, application);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(QSettings::defaultFormat(), QSettings::IniFormat);
    QSettings isolated;
    // Abort before any write if a regression selects the real native backend.
    QCOMPARE(isolated.format(), QSettings::IniFormat);
    for (const auto& key : {lastRoot, recent, repositories, sessions, draft})
        QVERIFY2(!isolated.contains(key), qPrintable(key));

    for (const auto& key : {lastRoot, recent, repositories, sessions, draft})
        isolated.setValue(key, isolatedValue);
    isolated.sync();
    QCOMPARE(isolated.status(), QSettings::NoError);
    const auto relativeFile = QStringLiteral("user/%1/%2.ini").arg(organization, application);
    QSettings persisted(QDir(isolatedPath).filePath(relativeFile), QSettings::IniFormat);
    for (const auto& key : {lastRoot, recent, repositories, sessions, draft}) {
        QCOMPARE(QSettings().value(key).toString(), isolatedValue);
        QCOMPARE(persisted.value(key).toString(), isolatedValue);
    }

#ifdef Q_OS_MACOS
    QCOMPARE(readCount, 0);
#endif
    userFallback.sync();
    systemFallback.sync();
    QCOMPARE(userFallback.value(recent).toString(), ordinaryValue);
    QCOMPARE(systemFallback.value(repositories).toString(), ordinaryValue);

    auto& updater = ketplus::AppUpdater::instance();
    updater.start();
    updater.checkForUpdates();
    QVERIFY(!updater.isConfigured());
    QVERIFY(!updater.canCheckForUpdates());
}

QTEST_GUILESS_MAIN(QaSettingsIsolationTest)

#include "QaSettingsIsolationTest.moc"
