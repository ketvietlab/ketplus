#include "cloud-privacy/CloudPrivacyTestAccess.h"
#include "cloud-privacy/FakeTransport.h"
#include "cloud-privacy/MemoryCredentials.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace ketplus::daemon;
namespace {
class DeviceTransport final : public QNetworkAccessManager {
  public:
    QList<QJsonObject> bodies;
    QList<QString> paths;
    QString approval =
        "https://synthetic.invalid/native/ketplus/v2/auth/continue?handoff=synthetic";
    QString reason = "authorization_pending";

  protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* outgoing) override {
        if (operation != CustomOperation || !outgoing)
            qFatal("Unexpected device authorization request");
        paths.append(request.url().path());
        bodies.append(QJsonDocument::fromJson(outgoing->readAll()).object());
        QJsonObject body;
        int status = 400;
        if (request.url().path().endsWith("/v2/auth/device/code")) {
            body = {{"device_code", "ketplus_device_" + QString(43, 'A')},
                    {"user_code", "A1B2C3D4"},
                    {"verification_uri_complete", approval},
                    {"expires_in", 600},
                    {"interval", 5}};
            status = 200;
        } else if (request.url().path().endsWith("/v2/auth/device/token")) {
            body = {{"error", reason}};
        } else
            qFatal("Fake device transport cannot enroll or reach a real relay");
        return new r03::Reply(request, status, QJsonDocument(body).toJson(QJsonDocument::Compact),
                              this);
    }
};
} // namespace
class DeviceAuthorizationTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { r03::resetCredentials(); }
    void pendingPollingUsesPkceAndTheSecretNeverLeavesLocalStatus() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.db"));
        CodexAdapter adapter("/nonexistent/codex");
        SessionHost host(&journal, &adapter);
        DeviceTransport transport;
        CloudControlClient client(&journal, &host);
        CloudPrivacyTestAccess::deviceTransport(client, transport);
        bool answered = false;
        QJsonObject receipt;
        client.handleLocal("remote.sign_in.device", {}, [&](auto value, auto failure) {
            QVERIFY2(failure.isEmpty(), qPrintable(failure));
            receipt = value;
            answered = true;
        });
        QTRY_VERIFY(answered);
        QVERIFY(receipt.value("ok").toBool());
        QVERIFY(!receipt.contains("device_code"));
        QVERIFY(!receipt.contains("code_verifier"));
        const auto started = transport.bodies.first();
        QCOMPARE(started.value("code_challenge").toString().size(), 43);
        CloudPrivacyTestAccess::pollDevice(client);
        QTRY_COMPARE(transport.bodies.size(), 2);
        QTRY_VERIFY(!CloudPrivacyTestAccess::devicePollInFlight(client));
        const auto polled = transport.bodies.last();
        QCOMPARE(polled.value("installation_id"), started.value("installation_id"));
        QCOMPARE(
            QString::fromLatin1(CloudCrypto::base64Url(QCryptographicHash::hash(
                polled.value("code_verifier").toString().toLatin1(), QCryptographicHash::Sha256))),
            started.value("code_challenge").toString());
        QTRY_VERIFY(CloudPrivacyTestAccess::status(client).value("sign_in_pending").toBool());
        QVERIFY(!QJsonDocument(CloudPrivacyTestAccess::status(client))
                     .toJson()
                     .contains("ketplus_device_"));
        transport.reason = "slow_down";
        CloudPrivacyTestAccess::pollDevice(client);
        QTRY_COMPARE(CloudPrivacyTestAccess::deviceInterval(client), 10000);
        transport.reason = "expired_token";
        CloudPrivacyTestAccess::pollDevice(client);
        QTRY_VERIFY(!CloudPrivacyTestAccess::status(client).value("sign_in_pending").toBool());
        QCOMPARE(CloudPrivacyTestAccess::status(client).value("sign_in_error").toString(),
                 "expired_token");
    }
    void aForeignApprovalOriginIsRefused() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.db"));
        CodexAdapter adapter("/nonexistent/codex");
        SessionHost host(&journal, &adapter);
        DeviceTransport transport;
        transport.approval = "https://attacker.example/approve";
        CloudControlClient client(&journal, &host);
        CloudPrivacyTestAccess::deviceTransport(client, transport);
        bool answered = false;
        client.handleLocal("remote.sign_in.device", {}, [&](auto, auto failure) {
            QVERIFY(!failure.isEmpty());
            answered = true;
        });
        QTRY_VERIFY(answered);
        QVERIFY(!CloudPrivacyTestAccess::status(client).value("sign_in_pending").toBool());
    }
    void cancellationFencesTheInitialReplyAndExpiryStopsPolling() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.db"));
        CodexAdapter adapter("/nonexistent/codex");
        SessionHost host(&journal, &adapter);
        DeviceTransport transport;
        CloudControlClient client(&journal, &host);
        CloudPrivacyTestAccess::deviceTransport(client, transport);
        bool canceled = false;
        client.handleLocal("remote.sign_in.device", {}, [&](auto, auto failure) {
            QVERIFY(!failure.isEmpty());
            canceled = true;
        });
        QVERIFY(CloudPrivacyTestAccess::status(client).value("sign_in_pending").toBool());
        client.handleLocal("remote.sign_in.cancel", {},
                           [](auto, auto failure) { QVERIFY(failure.isEmpty()); });
        QTRY_VERIFY(canceled);
        QVERIFY(!CloudPrivacyTestAccess::status(client).value("sign_in_pending").toBool());
        bool started = false;
        client.handleLocal("remote.sign_in.device", {}, [&](auto, auto failure) {
            QVERIFY(failure.isEmpty());
            started = true;
        });
        QTRY_VERIFY(started);
        CloudPrivacyTestAccess::expireDevice(client);
        const auto count = transport.paths.size();
        CloudPrivacyTestAccess::pollDevice(client);
        QCOMPARE(transport.paths.size(), count);
        QVERIFY(!CloudPrivacyTestAccess::status(client).value("sign_in_pending").toBool());
    }
};
QTEST_GUILESS_MAIN(DeviceAuthorizationTest)
#include "DeviceAuthorizationTest.moc"
