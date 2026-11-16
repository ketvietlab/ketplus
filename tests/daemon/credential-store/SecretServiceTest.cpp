#include "cloud/CredentialStore.h"
#include "cloud/SecretServiceCredentialBackend.h"
#include <QDBusMessage>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
using namespace ketplus::daemon;
class SecretServiceTest final : public QObject {
    Q_OBJECT
    QTemporaryDir busDirectory_{"/tmp/ketplus-dbus-XXXXXX"};
    QProcess bus_;
    QProcess service_;
    QString address_;
    QString name_{"synthetic-secret-service-test"};
    QDBusConnection connection_{name_};
    void configure(const QString& mode) {
        auto message = QDBusMessage::createMethodCall(
            "org.freedesktop.secrets", "/org/freedesktop/secrets", "dev.ketplus.Test", "Configure");
        message.setArguments({mode});
        QCOMPARE(connection_.call(message, QDBus::Block, 1000).type(), QDBusMessage::ReplyMessage);
    }
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        // An owned private bus: never connect these fixtures to the user's session service.
        bus_.start(QStringLiteral(DBUS_DAEMON), {"--session", "--nofork", "--print-address=1",
                                                 "--address=unix:tmpdir=" + busDirectory_.path()});
        QVERIFY2(bus_.waitForReadyRead(3000), bus_.readAllStandardError().constData());
        address_ = QString::fromUtf8(bus_.readLine()).trimmed();
        QVERIFY(address_.startsWith("unix:"));
        connection_ = QDBusConnection::connectToBus(address_, name_);
        QVERIFY(connection_.isConnected());
    }
    void init() {
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("DBUS_SESSION_BUS_ADDRESS", address_);
        service_.setProcessEnvironment(env);
        service_.start(QStringLiteral(SECRET_SERVICE_FIXTURE), {});
        QVERIFY(service_.waitForReadyRead(3000));
        QCOMPARE(service_.readLine().trimmed(), "ready");
    }
    void cleanup() {
        service_.terminate();
        if (!service_.waitForFinished(3000)) {
            service_.kill();
            service_.waitForFinished(3000);
        }
    }
    void roundTripLockAndWriteFailure() {
        QTemporaryDir dir;
        CredentialStore store("synthetic", secretServiceCredentialBackend(connection_),
                              dir.filePath("legacy.json"));
        QString error;
        QVERIFY2(store.write("private-key", "synthetic", &error), qPrintable(error));
        QCOMPARE(store.read("private-key", &error), "synthetic");
        configure("locked");
        QVERIFY(store.read("private-key", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!store.write("private-key", "replacement", &error));
        configure("denied");
        QVERIFY(!store.remove("private-key", &error));
        configure({});
        QCOMPARE(store.read("private-key"), "synthetic");
        QVERIFY(store.remove("private-key", &error));
        QVERIFY(store.read("private-key").isEmpty());
        QVERIFY(!QFile::exists(dir.filePath("legacy.json")));
    }
    void promptsAreDismissedWithoutBeingDisplayed() {
        QTemporaryDir dir;
        CredentialStore store("synthetic", secretServiceCredentialBackend(connection_),
                              dir.filePath("legacy.json"));
        configure("prompt");
        QString error;
        QVERIFY(!store.write("private-key", "synthetic", &error));
        QVERIFY(error.contains("interactive"));
        auto message = QDBusMessage::createMethodCall(
            "org.freedesktop.secrets", "/org/freedesktop/secrets", "dev.ketplus.Test", "Counts");
        const auto counts = connection_.call(message, QDBus::Block, 1000).arguments();
        QCOMPARE(counts.size(), 2);
        QCOMPARE(counts.at(0).toInt(), 0);
        QCOMPARE(counts.at(1).toInt(), 1);
        configure({});
        QVERIFY2(store.write("private-key", "synthetic", &error), qPrintable(error));
        QCOMPARE(store.read("private-key"), "synthetic");
    }
    void malformedReplyIsNotMissingStorage() {
        QTemporaryDir dir;
        CredentialStore store("synthetic", secretServiceCredentialBackend(connection_),
                              dir.filePath("legacy.json"));
        configure("malformed");
        QString error;
        QVERIFY(store.read("key", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!store.write("key", "replacement", &error));
        configure({});
        QVERIFY(store.read("key", &error).isEmpty());
        QVERIFY(error.isEmpty());
    }
    void absentServiceFailsClosed() {
        service_.terminate();
        QVERIFY(service_.waitForFinished(3000));
        QTemporaryDir dir;
        CredentialStore store("synthetic", secretServiceCredentialBackend(connection_),
                              dir.filePath("legacy.json"));
        QString error;
        QVERIFY(store.read("key", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!store.write("key", "synthetic", &error));
        QVERIFY(!QFile::exists(dir.filePath("legacy.json")));
    }
    void cleanupTestCase() {
        QDBusConnection::disconnectFromBus(name_);
        bus_.terminate();
        if (!bus_.waitForFinished(3000)) {
            bus_.kill();
            bus_.waitForFinished(3000);
        }
    }
};
QTEST_GUILESS_MAIN(SecretServiceTest)
#include "SecretServiceTest.moc"
