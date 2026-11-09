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
    void tunnelsCloseWithTheirDeviceAndConversation() {
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
        // An enrolled machine; revocations reach it only then.
        signaling->configure("workspace", "machine", 1, true);
        signaling->receive({{"type", "device.revoked"}, {"deviceId", "phone-a"}});
        QTRY_VERIFY(!revokedPhone);
        QVERIFY(samePhoneOtherTab && otherConversation && newerGeneration);
        emit host.eventReady({{"event_type", "session.deleted"},
                              {"session_id", "first"},
                              {"session_generation", 1}});
        QTRY_VERIFY(!samePhoneOtherTab);
        QVERIFY(otherConversation && newerGeneration);
    }
};

QTEST_GUILESS_MAIN(AtlasAccessTest)
#include "AtlasAccessTest.moc"
