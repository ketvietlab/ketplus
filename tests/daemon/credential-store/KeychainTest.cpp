#include "cloud/CredentialStore.h"
#include "cloud/KeychainCredentialBackend.h"
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
using namespace ketplus::daemon;
class KeychainTest final : public QObject {
    Q_OBJECT
    QTemporaryDir directory_;
    SecKeychainRef keychain_{nullptr};
    const QByteArray password_{"synthetic-private-keychain-password-7a4d"};
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(directory_.isValid());
        const auto path = QFile::encodeName(directory_.filePath("isolated.keychain"));
        QCOMPARE(SecKeychainCreate(path.constData(), password_.size(), password_.constData(), false,
                                   nullptr, &keychain_),
                 errSecSuccess);
    }
    void roundTripMigrationLockAndRetry() {
        Boolean interactionBefore = false;
        QCOMPARE(SecKeychainGetUserInteractionAllowed(&interactionBefore), errSecSuccess);
        const auto backend = keychainCredentialBackend(keychain_);
        const auto legacy = directory_.filePath("legacy.json");
        const QByteArray secret("synthetic-long-lived-private-key-never-a-real-credential-7319");
        QFile file(legacy);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
        const auto bytes =
            QJsonDocument(QJsonObject{{"private-key", QString::fromLatin1(secret.toBase64())},
                                      {"machine-id", "c3ludGhldGlj"}})
                .toJson();
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        CredentialStore store("native-fixture", backend, legacy);
        QString error;
        QCOMPARE(store.read("private-key", &error), secret);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!QFile::exists(legacy));
        QVERIFY(store.write("pin", "synthetic-pin", &error));
        QCOMPARE(SecKeychainLock(keychain_), errSecSuccess);
        QVERIFY(store.read("private-key", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!store.remove("machine-id", &error));
        QCOMPARE(SecKeychainUnlock(keychain_, password_.size(), password_.constData(), true),
                 errSecSuccess);
        QCOMPARE(store.read("machine-id"), "synthetic");
        QVERIFY(store.removeIf([](const auto& key) { return key != "private-key"; }, &error));
        QVERIFY(store.read("machine-id").isEmpty());
        QCOMPARE(store.read("private-key"), secret);
        Boolean interactionAfter = false;
        QCOMPARE(SecKeychainGetUserInteractionAllowed(&interactionAfter), errSecSuccess);
        QCOMPARE(interactionAfter, interactionBefore);
        // Inspect only the disposable keychain's encrypted on-disk copy.
        QDir dir(directory_.path());
        for (const auto& name : dir.entryList(QDir::Files)) {
            QFile candidate(dir.filePath(name));
            QVERIFY(candidate.open(QIODevice::ReadOnly));
            const auto content = candidate.readAll();
            QVERIFY(!content.contains(secret));
            QVERIFY(!content.contains(secret.toBase64()));
        }
    }
    void representativeCatalogRoundTrip() {
        CredentialStore store("catalog-fixture", keychainCredentialBackend(keychain_),
                              directory_.filePath("catalog-legacy.json"));
        QString error;
        QVERIFY2(store.update(
                     [](auto& accounts) {
                         for (int index = 0; index < 1024; ++index)
                             accounts.insert(QStringLiteral("synthetic-session-key-%1").arg(index),
                                             QByteArray(32, 'x'));
                     },
                     &error),
                 qPrintable(error));
        QElapsedTimer timer;
        timer.start();
        for (int index = 0; index < 100; ++index)
            QCOMPARE(store.read(QStringLiteral("synthetic-session-key-%1").arg(index)),
                     QByteArray(32, 'x'));
        qInfo("100 native Keychain reads from a 1024-account synthetic catalog: %lld ms",
              static_cast<long long>(timer.elapsed()));
    }
    void differentExecutableCannotReadWithoutApproval() {
        QProcess probe;
        probe.start(QStringLiteral(KEYCHAIN_PROBE), {directory_.filePath("isolated.keychain")});
        QVERIFY(probe.waitForFinished(10000));
        QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
        QCOMPARE(probe.exitCode(), 0);
    }
    void cleanupTestCase() {
        if (keychain_) {
            QCOMPARE(SecKeychainUnlock(keychain_, password_.size(), password_.constData(), true),
                     errSecSuccess);
            const auto status = SecKeychainDelete(keychain_);
            CFRelease(keychain_);
            keychain_ = nullptr;
            QCOMPARE(status, errSecSuccess);
        }
    }
};
QTEST_GUILESS_MAIN(KeychainTest)
#include "KeychainTest.moc"
