#include "cloud/CredentialStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtTest>

namespace ketplus::daemon {
class CredentialStoreTestAccess final {
  public:
    static int fileReads(const CredentialStore& store) { return store.fileReads_; }
    static int fileWrites(const CredentialStore& store) { return store.fileWrites_; }
};
} // namespace ketplus::daemon

using namespace ketplus::daemon;

namespace {
QString credentialFile(const QString& service) {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("Ketsuite/KetPlus/daemon/credentials-%1.json").arg(service));
}
} // namespace

class CredentialStoreTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void storesBinaryValuesInOwnerOnlyFile() {
        const QString service = QStringLiteral("credential-store-roundtrip");
        QFile::remove(credentialFile(service));
        CredentialStore store(service);
        QByteArray secret(32, Qt::Uninitialized);
        for (int index = 0; index < secret.size(); ++index)
            secret[index] = static_cast<char>(index * 7);

        QCOMPARE(store.read(QStringLiteral("missing")), QByteArray());
        QString error;
        QVERIFY2(store.write(QStringLiteral("signing-private-key"), secret, &error),
                 qPrintable(error));
        QVERIFY(store.write(QStringLiteral("machine-id"), "machine"));
        QCOMPARE(store.read(QStringLiteral("signing-private-key")), secret);
        QCOMPARE(CredentialStore(service).read(QStringLiteral("machine-id")),
                 QByteArray("machine"));
#if defined(Q_OS_UNIX)
        QCOMPARE(QFileInfo(credentialFile(service)).permissions() &
                     (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup |
                      QFileDevice::WriteOther),
                 QFileDevice::Permissions());
#endif

        QVERIFY(store.remove(QStringLiteral("machine-id")));
        QVERIFY(store.remove(QStringLiteral("machine-id")));
        QCOMPARE(store.read(QStringLiteral("machine-id")), QByteArray());
        QCOMPARE(store.read(QStringLiteral("signing-private-key")), secret);
    }

    void removesMatchingAccountsWithOneWrite() {
        const QString service = QStringLiteral("credential-store-batch");
        QFile::remove(credentialFile(service));
        CredentialStore store(service);
        for (int session = 0; session < 5; ++session)
            QVERIFY(store.write(QStringLiteral("uploaded-cursor-%1-1").arg(session), "7"));
        QVERIFY(store.write(QStringLiteral("machine-id"), "machine-a"));
        const int writes = CredentialStoreTestAccess::fileWrites(store);
        QVERIFY(store.removeIf([](const QString& account) {
            return account.startsWith(QStringLiteral("uploaded-cursor-"));
        }));
        QCOMPARE(CredentialStoreTestAccess::fileWrites(store), writes + 1);
        QCOMPARE(CredentialStore(service).read(QStringLiteral("uploaded-cursor-3-1")),
                 QByteArray());
        QCOMPARE(CredentialStore(service).read(QStringLiteral("machine-id")),
                 QByteArray("machine-a"));

        // Nothing to remove leaves the file alone.
        QVERIFY(store.removeIf([](const QString&) { return false; }));
        QVERIFY(store.remove(QStringLiteral("uploaded-cursor-3-1")));
        QCOMPARE(CredentialStoreTestAccess::fileWrites(store), writes + 1);
    }

    void readsTheFileAgainOnlyWhenItChanges() {
        const QString service = QStringLiteral("credential-store-snapshot");
        const QString path = credentialFile(service);
        QFile::remove(path);
        CredentialStore store(service);
        QVERIFY(store.write(QStringLiteral("machine-id"), "machine-a"));
        for (int read = 0; read < 3; ++read)
            QCOMPARE(store.read(QStringLiteral("machine-id")), QByteArray("machine-a"));
        QCOMPARE(CredentialStoreTestAccess::fileReads(store), 0);

        // Another store's same-size write, in the same millisecond as the last one, is seen.
        QVERIFY(CredentialStore(service).write(QStringLiteral("machine-id"), "machine-b"));
        QCOMPARE(store.read(QStringLiteral("machine-id")), QByteArray("machine-b"));
        QCOMPARE(store.read(QStringLiteral("machine-id")), QByteArray("machine-b"));
        QCOMPARE(CredentialStoreTestAccess::fileReads(store), 1);

        // So is a rewrite in place that restores the modification time.
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        const QDateTime modified = file.fileTime(QFileDevice::FileModificationTime);
        QByteArray bytes = file.readAll();
        const qsizetype value = bytes.indexOf(QByteArray("machine-b").toBase64());
        QVERIFY(value >= 0);
        bytes.replace(value, 12, QByteArray("machine-c").toBase64());
        QVERIFY(file.seek(0));
        QCOMPARE(file.write(bytes), bytes.size());
        QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
        file.close();
        QCOMPARE(store.read(QStringLiteral("machine-id")), QByteArray("machine-c"));

        // The file's safety is checked on every read, not only when it is parsed.
#if defined(Q_OS_UNIX)
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                QFileDevice::ReadOther));
        QString error;
        QCOMPARE(store.read(QStringLiteral("machine-id"), &error), QByteArray());
        QVERIFY(!error.isEmpty());
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QCOMPARE(store.read(QStringLiteral("machine-id")), QByteArray("machine-c"));
#endif
        QVERIFY(QFile::remove(path));
        QCOMPARE(store.read(QStringLiteral("machine-id")), QByteArray());
    }

    void corruptFileIsAnErrorAndIsNeverOverwritten() {
        const QString service = QStringLiteral("credential-store-corrupt");
        const QString path = credentialFile(service);
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write("{not json") > 0);
        file.close();
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));

        CredentialStore store(service);
        QString error;
        QCOMPARE(store.read(QStringLiteral("signing-private-key"), &error), QByteArray());
        QVERIFY(!error.isEmpty());
        error.clear();
        QVERIFY(!store.write(QStringLiteral("signing-private-key"), "replacement", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("{not json"));
    }
};

QTEST_GUILESS_MAIN(CredentialStoreTest)
#include "CredentialStoreTest.moc"
