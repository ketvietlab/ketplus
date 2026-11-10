#include "atlas/AtlasLiveRuntime.h"
#include "cloud-privacy/CloudPrivacyTestAccess.h"
#include "cloud-privacy/FakeTransport.h"
#include "cloud-privacy/MemoryCredentials.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "transport/Signaling.h"
#include <QPointer>
#include <QTemporaryDir>
#include <QtTest>
using namespace ketplus::daemon;

// A live Atlas is opened for one device and one conversation; it must not outlive either.
class AtlasAccessTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { r03::resetCredentials(); }
    void tunnelsCloseWithTheirDeviceAndConversation_data() {
        QTest::addColumn<bool>("peerTransport");
        QTest::newRow("peer transport") << true;
        // Revocation does not wait for WebRTC to be negotiated.
        QTest::newRow("no peer transport") << false;
    }
    void tunnelsCloseWithTheirDeviceAndConversation() {
        QFETCH(bool, peerTransport);
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.db"));
        CodexAdapter adapter("/nonexistent/codex");
        SessionHost host(&journal, &adapter);
        CloudControlClient client(&journal, &host);
        const auto open = [&](const char* device, const char* session, qint64 generation) {
            auto* runtime = new AtlasLiveRuntime(&client);
            runtime->setProperty("device", device);
            runtime->setProperty("session", session);
            runtime->setProperty("generation", generation);
            return QPointer<AtlasLiveRuntime>(runtime);
        };
        const auto revokedPhone = open("phone-a", "first", 1);
        const auto samePhoneOtherTab = open("phone-b", "first", 1);
        const auto otherConversation = open("phone-b", "second", 1);
        const auto newerGeneration = open("phone-b", "first", 2);
        auto* signaling = client.findChild<transport::Signaling*>();
        QVERIFY(signaling);
        signaling->configure("workspace", "machine", 1, peerTransport);
        signaling->receive({{"type", "device.revoked"}, {"deviceId", "phone-a"}});
        QTRY_VERIFY(!revokedPhone);
        QVERIFY(samePhoneOtherTab && otherConversation && newerGeneration);
        emit host.eventReady({{"event_type", "session.deleted"},
                              {"session_id", "first"},
                              {"session_generation", 1}});
        QTRY_VERIFY(!samePhoneOtherTab);
        QVERIFY(otherConversation && newerGeneration);
    }
    // An atlas.start captures this fence before its scan and creates no runtime once it moved.
    void startsInFlightEndWithTheirAccess() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.db"));
        CodexAdapter adapter("/nonexistent/codex");
        SessionHost host(&journal, &adapter);
        CloudControlClient client(&journal, &host);
        using Access = CloudPrivacyTestAccess;
        const auto phoneA = Access::atlasFence(client, "phone-a");
        const auto phoneB = Access::atlasFence(client, "phone-b");
        auto* signaling = client.findChild<transport::Signaling*>();
        QVERIFY(signaling);
        signaling->configure("workspace", "machine", 1, false);
        signaling->receive({{"type", "device.revoked"}, {"deviceId", "phone-a"}});
        QVERIFY(!Access::atlasAccessCurrent(client, "phone-a", phoneA));
        QVERIFY(Access::atlasAccessCurrent(client, "phone-b", phoneB));
        // A start begun after the revocation is not fenced by it.
        QVERIFY(
            Access::atlasAccessCurrent(client, "phone-a", Access::atlasFence(client, "phone-a")));
        emit Access::wakeups(client).authorizationRevoked();
        QVERIFY(!Access::atlasAccessCurrent(client, "phone-b", phoneB));
    }
};

QTEST_GUILESS_MAIN(AtlasAccessTest)
#include "AtlasAccessTest.moc"
