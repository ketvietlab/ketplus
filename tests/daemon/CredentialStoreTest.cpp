#include "cloud/CredentialStore.h"
#include "cloud/CredentialBackend.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#if defined(Q_OS_UNIX)
#include <sys/file.h>
#endif
using namespace ketplus::daemon;
namespace {
class Backend final : public CredentialBackend {
  public:
    QByteArray record;
    bool exists{false};
    bool locked{false};
    bool failWrite{false};
    bool failVerification{false};
    int writes{0};
    int reads{0};
    std::function<void()> afterWrite;
    Result read(const QString&, QByteArray* value, QString* error) override {
        ++reads;
        if (locked || (failVerification && writes > 0)) {
            if (error)
                *error = "Synthetic store unavailable";
            return Result::Error;
        }
        *value = record;
        return exists ? Result::Found : Result::Missing;
    }
    bool write(const QString&, const QByteArray& value, QString* error) override {
        if (locked || failWrite) {
            if (error)
                *error = "Synthetic store write denied";
            return false;
        }
        ++writes;
        exists = true;
        record = value;
        if (afterWrite)
            afterWrite();
        return true;
    }
    bool accessible(const QString&, QString* error) override {
        if (locked && error)
            *error = "Synthetic store unavailable";
        return !locked;
    }
};
struct Fixture {
    QTemporaryDir directory;
    QString path{directory.filePath("credentials-test.json")};
    std::shared_ptr<Backend> backend{std::make_shared<Backend>()};
    CredentialStore store{"test", backend, path};
    QByteArray legacyBytes{
        QJsonDocument(QJsonObject{{"private-key", "c3ludGhldGlj"}, {"machine-id", "bWFjaGluZQ=="}})
            .toJson()};
    void legacy(const QByteArray& bytes = {}) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) ||
            !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
            file.write(bytes.isEmpty() ? legacyBytes : bytes) < 0)
            qFatal("Unable to create synthetic legacy fixture");
    }
    QByteArray onDisk() {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }
};
} // namespace
class CredentialStoreTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void productionBackendIsDisabledInTests() {
        QString error;
        CredentialStore store("synthetic-no-native-access");
        QVERIFY(store.read("key", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!store.write("key", "synthetic", &error));
    }
    void roundTripWithoutPlaintextFile() {
        Fixture f;
        QString error;
        QVERIFY(f.store.read("missing", &error).isEmpty());
        QVERIFY(error.isEmpty());
        const QByteArray binary("synthetic\0key", 13);
        QVERIFY(f.store.write("private-key", binary, &error));
        QCOMPARE(f.store.read("private-key"), binary);
        QCOMPARE(CredentialStore("test", f.backend, f.path).read("private-key"), binary);
        QVERIFY(!QFile::exists(f.path));
#if !defined(Q_OS_WIN) // Windows access is an ACL, not mode bits: see WindowsPlatformTest.
        QCOMPARE(QFileInfo(f.directory.path()).permissions() &
                     (QFile::ReadOther | QFile::WriteOther),
                 QFile::Permissions{});
#endif
    }
    void migrationIsVerifiedThenRemoved() {
        Fixture f;
        f.legacy();
        QCOMPARE(f.store.read("private-key"), "synthetic");
        QCOMPARE(f.backend->writes, 1);
        QVERIFY(!QFile::exists(f.path));
        QCOMPARE(f.store.read("machine-id"), "machine");
        QVERIFY(f.store.remove("machine-id"));
        // A backup from before sign-out must never resurrect an enrollment.
        f.legacy();
        QCOMPARE(f.store.read("machine-id"), QByteArray{});
        QCOMPARE(f.store.read("private-key"), "synthetic");
        QVERIFY(!QFile::exists(f.path));
        QCOMPARE(f.backend->writes, 2);
    }
    void unavailableStoreNeverReadsOrOverwritesLegacy() {
        Fixture f;
        f.legacy();
        f.backend->locked = true;
        QString error;
        QVERIFY(f.store.read("private-key", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!f.store.write("private-key", "replacement", &error));
        QCOMPARE(f.onDisk(), f.legacyBytes);
        QCOMPARE(f.backend->writes, 0);
        f.backend->locked = false;
        QCOMPARE(f.store.read("private-key"), "synthetic");
    }
    void migrationFailureRetriesWithoutLosingSource_data() {
        QTest::addColumn<bool>("verification");
        QTest::newRow("write") << false;
        QTest::newRow("read-back") << true;
    }
    void migrationFailureRetriesWithoutLosingSource() {
        QFETCH(bool, verification);
        Fixture f;
        f.legacy();
        f.backend->failWrite = !verification;
        f.backend->failVerification = verification;
        QString error;
        QVERIFY(f.store.read("private-key", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QCOMPARE(f.onDisk(), f.legacyBytes);
        f.backend->failWrite = f.backend->failVerification = false;
        QCOMPARE(f.store.read("private-key"), "synthetic");
        QVERIFY(!QFile::exists(f.path));
        QCOMPARE(f.backend->writes, 1);
    }
    void cleanupFailureRetriesTheCommittedMigration() {
#if defined(Q_OS_WIN)
        QSKIP("The failure is induced with POSIX directory permissions.");
#endif
        Fixture f;
        f.legacy();
        const auto restore = qScopeGuard([&] {
            QFile::setPermissions(f.directory.path(),
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        f.backend->afterWrite = [&] {
            QVERIFY(QFile::setPermissions(f.directory.path(), QFile::ReadOwner | QFile::ExeOwner));
        };
        QString error;
        QVERIFY(f.store.read("private-key", &error).isEmpty());
        QVERIFY(error.contains("cleanup"));
        QCOMPARE(f.onDisk(), f.legacyBytes);
        QCOMPARE(f.backend->writes, 1);
        QVERIFY(QFile::setPermissions(f.directory.path(),
                                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        f.backend->afterWrite = {};
        QCOMPARE(f.store.read("private-key", &error), "synthetic");
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!QFile::exists(f.path));
        QCOMPARE(f.backend->writes, 1);
    }
    void corruptOsRecordNeverFallsBackToLegacy() {
        Fixture f;
        f.legacy();
        f.backend->exists = true;
        f.backend->record = "not-json";
        QString error;
        QVERIFY(f.store.read("private-key", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!f.store.write("private-key", "replacement", &error));
        QCOMPARE(f.onDisk(), f.legacyBytes);
        QCOMPARE(f.backend->record, "not-json");
        QCOMPARE(f.backend->writes, 0);
    }
    void conflictingBackupRequiresExplicitRecovery() {
        Fixture f;
        f.legacy();
        QCOMPARE(f.store.read("private-key"), "synthetic");
        const auto committed = f.backend->record;
        f.legacy("{\"private-key\":\"b3RoZXI=\"}");
        const auto conflict = f.onDisk();
        QString error;
        QVERIFY(f.store.read("private-key", &error).isEmpty());
        QVERIFY(error.contains("conflict"));
        QCOMPARE(f.onDisk(), conflict);
        QCOMPARE(f.backend->record, committed);
    }
    void rejectsUnsafeOrCorruptLegacy_data() {
        QTest::addColumn<QString>("kind");
        for (const auto* kind : {"permissions", "symlink", "corrupt", "base64", "type"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void rejectsUnsafeOrCorruptLegacy() {
        QFETCH(QString, kind);
#if defined(Q_OS_WIN)
        if (kind == "permissions" || kind == "symlink")
            QSKIP("Mode bits and symbolic links are POSIX; Windows uses ACLs and .lnk shortcuts.");
#endif
        Fixture f;
        f.legacy();
        if (kind == "permissions")
            QVERIFY(QFile::setPermissions(f.path,
                                          QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther));
        else if (kind == "symlink") {
            QVERIFY(QFile::rename(f.path, f.path + ".target"));
            QVERIFY(QFile::link(f.path + ".target", f.path));
        } else if (kind == "corrupt")
            f.legacy("not-json");
        else if (kind == "base64")
            f.legacy("{\"key\":\"!bad!\"}");
        else
            f.legacy("{\"key\":42}");
        QString error;
        QVERIFY(f.store.read("key", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QCOMPARE(f.backend->writes, 0);
        QVERIFY(QFile::exists(f.path));
    }
    void failedSignOutBatchPreservesKeysAndEnrollment() {
        Fixture f;
        QVERIFY(f.store.update([](auto& accounts) {
            accounts.insert("machine-id", "synthetic-machine");
            accounts.insert("pin", "synthetic-pin");
            accounts.insert("private-key", "synthetic-key");
        }));
        const auto before = f.backend->record;
        f.backend->failWrite = true;
        QString error;
        const auto enrolled = [](const QString& account) { return account != "private-key"; };
        QVERIFY(!f.store.removeIf(enrolled, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(f.backend->record, before);
        f.backend->failWrite = false;
        QVERIFY(f.store.removeIf(enrolled));
        QVERIFY(f.store.read("machine-id").isEmpty());
        QCOMPARE(f.store.read("private-key"), "synthetic-key");
        QCOMPARE(f.backend->writes, 2);
        QVERIFY(f.store.removeIf(enrolled));
        QCOMPARE(f.backend->writes, 2);
    }
    void lockAndStoreFailuresAreNotMissing() {
#if defined(Q_OS_UNIX)
        Fixture f;
        QVERIFY(f.store.write("key", "synthetic"));
        QString error;
        QFile lock(f.path + ".lock");
        QVERIFY(lock.open(QIODevice::ReadWrite));
        QCOMPARE(::flock(lock.handle(), LOCK_EX | LOCK_NB), 0);
        QVERIFY(f.store.read("key", &error).isEmpty());
        QVERIFY(error.contains("busy"));
        lock.close();
        f.backend->locked = true;
        // A cached record is not served while the OS store refuses access.
        QVERIFY(f.store.read("key", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        f.backend->locked = false;
        QCOMPARE(f.store.read("key"), "synthetic");
#else
        QSKIP("Native credential locking is supported on macOS and Linux.");
#endif
    }
    void repeatedReadsDecryptTheRecordOnce() {
        Fixture f;
        QVERIFY(f.store.write("session-key-a", "synthetic-a"));
        QVERIFY(f.store.write("session-key-b", "synthetic-b"));
        const int reads = f.backend->reads;
        for (int i = 0; i < 100; ++i) {
            QString error;
            QCOMPARE(f.store.read(i % 2 ? "session-key-a" : "session-key-b", &error),
                     i % 2 ? "synthetic-a" : "synthetic-b");
            QVERIFY(error.isEmpty());
        }
        QCOMPARE(f.backend->reads, reads);
        QCOMPARE(f.backend->writes, 2);
    }
    void writeByAnotherStoreIsSeen() {
        Fixture f;
        QVERIFY(f.store.write("key", "first"));
        QCOMPARE(f.store.read("key"), "first");
        // Another process holds its own store over the same lock file and OS record.
        CredentialStore other("test", f.backend, f.path);
        QVERIFY(other.write("key", "second"));
        QCOMPARE(f.store.read("key"), "second");
        QVERIFY(other.remove("key"));
        QCOMPARE(f.store.read("key"), QByteArray{});
    }
    void failedWriteRereadsTheOsRecord() {
        Fixture f;
        QVERIFY(f.store.write("key", "committed"));
        f.backend->failWrite = true;
        QString error;
        QVERIFY(!f.store.write("key", "rejected", &error));
        QVERIFY(!error.isEmpty());
        f.backend->failWrite = false;
        const int reads = f.backend->reads;
        QCOMPARE(f.store.read("key"), "committed");
        QCOMPARE(f.backend->reads, reads + 1);
    }
    void unchangedUpdateKeepsTheCache() {
        Fixture f;
        QVERIFY(f.store.write("key", "synthetic"));
        const int reads = f.backend->reads;
        QVERIFY(f.store.update([](auto& accounts) { accounts.insert("key", "synthetic"); }));
        QVERIFY(f.store.batch([](CredentialStore&, QString*) { return false; }) == false);
        QCOMPARE(f.store.read("key"), "synthetic");
        QCOMPARE(f.backend->reads, reads);
        QCOMPARE(f.backend->writes, 1);
    }
};
QTEST_GUILESS_MAIN(CredentialStoreTest)
#include "CredentialStoreTest.moc"
