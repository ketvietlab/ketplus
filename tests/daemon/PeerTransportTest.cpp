#include "cloud-privacy/CloudPrivacyTestAccess.h"
#include "cloud-privacy/FakeTransport.h"
#include "cloud-privacy/MemoryCredentials.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "transport/SelectedPath.h"
#include "transport/Signaling.h"
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <rtc/rtc.hpp>

using namespace ketplus::daemon;
using namespace ketplus::daemon::transport;
using Access = CloudPrivacyTestAccess;

namespace {
class Link final : public PeerLink {
  public:
    QList<Packet> sent;
    bool blocked{false};
    bool stopped{false};
    bool send(const Packet& packet) override {
        if (blocked)
            return false;
        sent.append(packet);
        return true;
    }
    quint64 buffered(Channel) const override { return blocked ? SendQueue::highWater : 0; }
    void close() override { stopped = true; }
    void frame(const QJsonObject& frame) {
        emit received(Channel::Control, QJsonDocument(frame).toJson(QJsonDocument::Compact), false);
    }
    void hello(const QString& id) {
        frame({{"type", "hello"},
               {"connectionId", id},
               {"capabilities", QJsonArray{"transport.webrtc/1"}}});
    }
};
QJsonObject grant(const QString& id = "one", const QString& device = "device") {
    const auto now = QDateTime::currentDateTimeUtc();
    return {{"connectionId", id},
            {"deviceId", device},
            {"machineId", "machine-synthetic"},
            {"workspaceId", "workspace-synthetic"},
            {"ownerSubjectId", "owner"},
            {"credentialEpoch", 1},
            {"issuedAt", now.toString(Qt::ISODateWithMs)},
            {"expiresAt", now.addSecs(600).toString(Qt::ISODateWithMs)},
            {"capabilities", QJsonArray{"session.read", "session.rename", "session.delete"}}};
}
QJsonObject command() {
    return {{"id", "command-one"},
            {"idempotencyKey", "command-one"},
            {"kind", "session.rename"},
            {"capability", "session.rename"},
            {"workspaceId", "workspace-synthetic"},
            {"machineId", "machine-synthetic"},
            {"sessionId", "session"},
            {"sessionGeneration", 1},
            {"content", QJsonObject{{"senderDeviceId", "device"}, {"senderKeyVersion", 1}}}};
}
EventPage empty(const QString&, qint64 generation, qint64 cursor) {
    return {{}, cursor, generation, true, false};
}
} // namespace

class PeerTransportTest final : public QObject {
    Q_OBJECT
  private slots:
    void selectedPathLogContainsOnlyBoundedTypes() {
        const auto at = QDateTime::fromMSecsSinceEpoch(1000, Qt::UTC);
        QCOMPARE(selectedPathLog(rtc::Candidate::Type::Relayed, rtc::Candidate::Type::Host, 7,
                                 false, at),
                 QString("ice-path local=relay remote=host turn=udp epoch=7 event=open "
                         "at=1970-01-01T00:00:01.000Z"));
        const auto restarted = selectedPathLog(rtc::Candidate::Type::Host,
                                               static_cast<rtc::Candidate::Type>(999), 8, true, at);
        QVERIFY(
            restarted.contains("local=host remote=unknown turn=none epoch=8 event=ice-connected"));
    }
    void init() { r03::resetCredentials(); }
    void transfersAreBoundedOrderedAndExpire() {
        const QJsonObject frame{{"type", "command"}, {"payload", QString(200'000, 'x')}};
        const auto parts = FrameCodec::encode(frame, Channel::Control, 71);
        QVERIFY(parts.size() > 3);
        FrameCodec codec;
        QString error;
        QJsonObject result;
        for (const auto& part : parts) {
            QVERIFY(part.bytes.size() <= FrameCodec::messageLimit);
            result = codec.receive(part, 1, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
        }
        QCOMPARE(result, frame);
        QVERIFY(codec.receive(parts[1], 2, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(codec.receive(parts[0], 3, &error).isEmpty());
        QVERIFY(error.isEmpty());
        QVERIFY(codec.receive(parts[0], 4, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        for (quint64 id = 1; id <= 4; ++id) {
            codec.receive(FrameCodec::encode(frame, Channel::Control, id).first(), 10, &error);
            QVERIFY(error.isEmpty());
        }
        codec.receive(FrameCodec::encode(frame, Channel::Control, 5).first(), 10, &error);
        QVERIFY(!error.isEmpty());
        QVERIFY(codec.expire(30'011));
        QVERIFY(FrameCodec::encode(
                    {{"type", "event"}, {"value", QString(FrameCodec::transferLimit, 'x')}},
                    Channel::Events, 1)
                    .isEmpty());
    }
    void strictPriorityAndAtomicQueueLimit() {
        Link link;
        SendQueue queue;
        QVERIFY(queue.append({{Channel::Bulk, "bulk", true},
                              {Channel::Events, "events", false},
                              {Channel::Control, "control", false}}));
        link.blocked = true;
        queue.flush(link);
        QVERIFY(link.sent.isEmpty());
        link.blocked = false;
        queue.flush(link);
        QCOMPARE(link.sent.size(), 3);
        QCOMPARE(link.sent[0].bytes, QByteArray("control"));
        QCOMPARE(link.sent[1].bytes, QByteArray("events"));
        QVERIFY(!queue.append({{Channel::Control, QByteArray(256 * 1024 + 1, 'x'), false}}));
        QVERIFY(queue.empty());
    }
    void helloEpochSupersedeAndRevocation() {
        RemotePeers peers(empty);
        QVERIFY(!peers.ticking());
        auto* first = new Link;
        QVERIFY(peers.accept("one", "device", 1, grant(), first, 10));
        QVERIFY(peers.ticking());
        first->hello("one");
        QCOMPARE(QJsonDocument::fromJson(first->sent[0].bytes).object().value("epoch").toInteger(),
                 10);
        auto* second = new Link;
        QVERIFY(peers.accept("two", "device", 1, grant("two"), second, 11));
        QVERIFY(!first->stopped);
        second->hello("two");
        QVERIFY(first->stopped);
        QCOMPARE(peers.size(), 1);
        QCOMPARE(QJsonDocument::fromJson(second->sent[0].bytes).object().value("epoch").toInteger(),
                 11);
        peers.revoke("device");
        QVERIFY(second->stopped);
        QCOMPARE(peers.size(), 0);
        QVERIFY(!peers.ticking()); // no peers, no wakeups (#126)
    }
    void grantScopeCapabilityAndExpiry() {
        RemotePeers peers(empty);
        auto* link = new Link;
        QVERIFY(peers.accept("one", "device", 1, grant(), link, 1));
        QVERIFY(!peers.authorized("one", command()));
        link->hello("one");
        QVERIFY(peers.authorized("one", command()));
        auto foreign = command();
        foreign.insert("workspaceId", "foreign");
        QVERIFY(!peers.authorized("one", foreign));
        auto wrong = command();
        wrong.insert("kind", "session.delete");
        wrong.insert("capability", "session.delete");
        QVERIFY(!peers.authorized("one", wrong));
        wrong = command();
        wrong.insert("capability", "session.read");
        QVERIFY(!peers.authorized("one", wrong));
        auto expired = grant();
        expired.insert("expiresAt",
                       QDateTime::currentDateTimeUtc().addSecs(-1).toString(Qt::ISODateWithMs));
        QVERIFY(!peers.renew("one", expired));
        auto expiring = grant();
        expiring.insert("expiresAt",
                        QDateTime::currentDateTimeUtc().addMSecs(30).toString(Qt::ISODateWithMs));
        QVERIFY(peers.renew("one", expiring));
        QTRY_COMPARE_WITH_TIMEOUT(peers.size(), 0, 1000);
    }
    void resumeAndLiveSubscribersAreFair() {
        QHash<QString, qint64> cursors;
        RemotePeers peers([&](const QString& session, qint64 generation, qint64 cursor) {
            cursors.insert(session, cursor);
            return EventPage{
                {QJsonObject{{"cursor", cursor + 1}}}, cursor + 1, generation, true, false};
        });
        auto* link = new Link;
        QVERIFY(peers.accept("one", "device", 1, grant(), link, 1));
        link->hello("one");
        link->frame({{"type", "subscribe"},
                     {"sessionId", "a"},
                     {"sessionGeneration", 1},
                     {"afterCursor", 4}});
        link->frame({{"type", "subscribe"},
                     {"sessionId", "b"},
                     {"sessionGeneration", 1},
                     {"afterCursor", 9}});
        peers.tick();
        peers.tick();
        QCOMPARE(cursors.value("a"), 4);
        QCOMPARE(cursors.value("b"), 9);
        peers.tick();
        peers.tick();
        QCOMPARE(cursors.value("a"),
                 4); // A caught-up subscription does no journal reads while idle.
        peers.eventsChanged("a");
        peers.eventsChanged("b");
        peers.tick();
        peers.tick();
        QCOMPARE(cursors.value("a"), 5);
        link->frame({{"type", "unsubscribe"}, {"sessionId", "a"}});
        peers.tick();
        peers.tick();
        QCOMPARE(cursors.value("a"), 5);
    }
    void signaturesBindOfferAnswerAndPairing() {
        CredentialStore store("peer-signature-test");
        CloudCrypto crypto(&store);
        const auto key =
            CloudCrypto::fromBase64Url(crypto.publicKeys().value("signingPublicKey").toString());
        QJsonObject offer{{"connectionId", "pc_abcdefghijklmnopqrstuvwx"},
                          {"deviceId", "device"},
                          {"deviceKeyVersion", 1},
                          {"grant", grant("pc_abcdefghijklmnopqrstuvwx")},
                          {"issuedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                          {"sdp", "fingerprint-original"}};
        const auto signedBytes = Signaling::offerBytes(offer);
        const auto signature = crypto.sign(signedBytes);
        QVERIFY(Signaling::verify(signedBytes, key, signature));
        offer.insert("sdp", "fingerprint-rewritten");
        QVERIFY(!Signaling::verify(Signaling::offerBytes(offer), key, signature));
        const auto answer = Signaling::answerBytes(offer, "answer", 1, "time");
        const auto signedAnswer = crypto.sign(answer);
        offer.insert("sdp", "different-offer");
        QVERIFY(!Signaling::verify(Signaling::answerBytes(offer, "answer", 1, "time"), key,
                                   signedAnswer));
    }
    void invalidOffersAndReplaysNeverCreatePeers() {
        RemotePeers peers(empty);
        int reads = 0;
        std::function<void(const QJsonObject&)> finish;
        Signaling signaling(
            &peers,
            [&](const QString&, int, auto callback) {
                ++reads;
                finish = callback;
            },
            [](const QByteArray&) { return QByteArray{}; });
        signaling.configure("workspace-synthetic", "machine-synthetic", 1, true);
        QSignalSpy sent(&signaling, &Signaling::outgoing);
        const QString id = "pc_abcdefghijklmnopqrstuvwx";
        QJsonObject offer{{"type", "signal.offer"},
                          {"connectionId", id},
                          {"deviceId", "device"},
                          {"deviceKeyVersion", 1},
                          {"grant", grant(id)},
                          {"sdp", "invalid"},
                          {"issuedAt", QDateTime::currentDateTimeUtc().addSecs(-301).toString(
                                           Qt::ISODateWithMs)}};
        signaling.receive(offer);
        QCOMPARE(reads, 0);
        offer.insert("issuedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        signaling.receive(offer);
        QCOMPARE(reads, 1);
        finish(
            {{"id", "device"}, {"keyVersion", 1}, {"status", "active"}, {"keys", QJsonObject{}}});
        QCOMPARE(peers.size(), 0);
        signaling.receive(offer);
        QCOMPARE(reads, 1); // Invalid/replayed offers cannot create additional key fetches.
        QVERIFY(sent.size() >= 3);
    }

    void signedNativePairCarriesFramesAndBulk() {
        // Exercise the real native open callback, not only the formatter.
        QTest::ignoreMessage(
            QtInfoMsg,
            QRegularExpression("^ice-path local=(host|srflx|prflx|relay|unknown) "
                               "remote=(host|srflx|prflx|relay|unknown) turn=(udp|none|unknown) "
                               "epoch=[0-9]+ event=open at=[0-9TZ:.-]+$"));
        const QString id = "pc_abcdefghijklmnopqrstuvwx";
        CredentialStore hostStore("native-host"), deviceStore("native-device");
        CloudCrypto hostCrypto(&hostStore), deviceCrypto(&deviceStore);
        const auto keys = deviceCrypto.publicKeys();
        const auto hostKey = CloudCrypto::fromBase64Url(
            hostCrypto.publicKeys().value("signingPublicKey").toString());
        RemotePeers peers(empty);
        Signaling signaling(
            &peers,
            [&](const QString&, int, auto callback) {
                callback(
                    {{"id", "device"}, {"keyVersion", 1}, {"status", "active"}, {"keys", keys}});
            },
            [&](const QByteArray& bytes) { return hostCrypto.sign(bytes); });
        signaling.configure("workspace-synthetic", "machine-synthetic", 1, true);
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        config.maxMessageSize = FrameCodec::messageLimit;
        auto client = std::make_shared<rtc::PeerConnection>(config);
        QObject context;
        QJsonObject offer;
        bool answerVerified = false;
        client->onLocalDescription([&](rtc::Description description) {
            const auto sdp = QString::fromStdString(std::string(description));
            QMetaObject::invokeMethod(
                &context,
                [&, sdp] {
                    offer = {
                        {"type", "signal.offer"},
                        {"connectionId", id},
                        {"deviceId", "device"},
                        {"deviceKeyVersion", 1},
                        {"grant", grant(id)},
                        {"sdp", sdp},
                        {"issuedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
                    offer.insert("signature",
                                 QString::fromLatin1(CloudCrypto::base64Url(
                                     deviceCrypto.sign(Signaling::offerBytes(offer)))));
                    signaling.receive(offer);
                },
                Qt::QueuedConnection);
        });
        client->onLocalCandidate([&](rtc::Candidate candidate) {
            const auto text = QString::fromStdString(std::string(candidate));
            const auto mid = QString::fromStdString(candidate.mid());
            QMetaObject::invokeMethod(
                &context,
                [&, text, mid] {
                    signaling.receive({{"type", "signal.candidate"},
                                       {"connectionId", id},
                                       {"candidate", text},
                                       {"sdpMid", mid}});
                },
                Qt::QueuedConnection);
        });
        connect(&signaling, &Signaling::outgoing, &context, [&](const QJsonObject& message) {
            if (message.value("type") == "signal.answer") {
                const auto sdp = message.value("sdp").toString();
                answerVerified = Signaling::verify(
                    Signaling::answerBytes(offer, sdp, 1, message.value("issuedAt").toString()),
                    hostKey, CloudCrypto::fromBase64Url(message.value("signature").toString()));
                if (answerVerified)
                    client->setRemoteDescription(rtc::Description(sdp.toStdString(), "answer"));
            } else if (message.value("type") == "signal.candidate" &&
                       message.value("candidate").isString()) {
                client->addRemoteCandidate(
                    rtc::Candidate(message.value("candidate").toString().toStdString(),
                                   message.value("sdpMid").toString().toStdString()));
            }
        });
        auto control = client->createDataChannel("control");
        auto events = client->createDataChannel("events");
        auto bulk = client->createDataChannel("bulk");
        std::atomic<bool> welcome{false};
        control->onMessage([&](rtc::message_variant value) {
            if (auto text = std::get_if<std::string>(&value))
                if (QJsonDocument::fromJson(QByteArray::fromStdString(*text))
                        .object()
                        .value("type") == "welcome")
                    welcome = true;
        });
        client->setLocalDescription();
        QTRY_VERIFY_WITH_TIMEOUT(answerVerified, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(control->isOpen() && events->isOpen() && bulk->isOpen(), 10'000);
        control->send(QJsonDocument(QJsonObject{{"type", "hello"},
                                                {"connectionId", id},
                                                {"capabilities", QJsonArray{"transport.webrtc/1"}}})
                          .toJson(QJsonDocument::Compact)
                          .toStdString());
        QTRY_VERIFY_WITH_TIMEOUT(welcome.load(), 3000);
        std::atomic<int> bytes{0};
        bulk->onMessage([&](rtc::message_variant message) {
            if (auto data = std::get_if<rtc::binary>(&message))
                bytes += int(data->size());
        });
        QVERIFY(peers.send(id, {{"type", "command.result"}, {"payload", QString(150'000, 'x')}}));
        QTRY_VERIFY_WITH_TIMEOUT(bytes.load() > 150'000, 3000);
        signaling.receive({{"type", "device.revoked"}, {"deviceId", "device"}});
        QCOMPARE(peers.size(), 0);
        client->resetCallbacks();
        control->resetCallbacks();
        bulk->resetCallbacks();
        events->resetCallbacks();
        client->close();
    }
    void encryptedProjectionAndCrossPathCommandDeduplication() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite"));
        StoredSession session;
        session.id = "session";
        session.worktreePath = dir.path();
        session.state = "ready";
        session.adapter = "codex";
        QVERIFY(journal.upsertSession(session));
        CodexAdapter provider(dir.filePath("never-run-provider"));
        SessionHost host(&journal, &provider);
        r03::Transport network;
        CloudControlClient cloud(&journal, &host);
        Access::configure(cloud, network);
        CredentialStore deviceStore("peer-device");
        CloudCrypto device(&deviceStore);
        const auto deviceKeys = device.publicKeys();
        Access::pinDevice(cloud, "device", 1, deviceKeys);
        network.statusForPath.insert("/v2/device-keys/device/1", 200);
        network.responseForPath.insert("/v2/device-keys/device/1",
                                       QJsonDocument(QJsonObject{{"keys", deviceKeys}}).toJson());
        const auto machineKey = CloudCrypto::fromBase64Url(
            Access::crypto(cloud).publicKeys().value("signingPublicKey").toString());
        const QJsonObject recipient{{"id", "device"}, {"keyVersion", 1}, {"keys", deviceKeys}};
        auto wrapped = Access::crypto(cloud).wrapSessionKey("session", 1, 1, recipient,
                                                            "machine-synthetic", 1);
        QString error;
        QVERIFY2(device.importSessionKey(wrapped, machineKey, "device", 1, &error),
                 qPrintable(error));
        auto sealed = command();
        auto shell = sealed.value("content").toObject();
        shell.insert("keyId", "session:1:1");
        shell.insert("keyEpoch", 1);
        sealed.insert("content", shell);
        sealed.insert("content", device.encrypt("{\"title\":\"Renamed once\"}",
                                                Access::commandAad(cloud, sealed), "session", 1, 1,
                                                "device", 1, &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        auto* link = new Link;
        auto& peers = Access::peers(cloud);
        QVERIFY(peers.accept("one", "device", 1, grant(), link, 1));
        link->hello("one");
        link->frame({{"type", "command"}, {"command", sealed}});
        QTRY_VERIFY_WITH_TIMEOUT(link->sent.size() >= 2, 3000);
        const auto result = QJsonDocument::fromJson(link->sent[1].bytes).object();
        QCOMPARE(result.value("type").toString(), "command.result");
        QCOMPARE(result.value("result").toObject().value("status").toString(), "completed");
        const auto first = journal.eventsAfter("session", 1, 0);
        QVERIFY(!first.isEmpty());
        sealed.insert("attempt", 1);
        sealed.insert("claimToken", "synthetic");
        Access::execute(cloud, sealed); // Same ciphertext delivered by the mailbox after path loss.
        QCOMPARE(journal.eventsAfter("session", 1, 0).size(), first.size());
        const auto page = Access::peerEvents(cloud, "session", 1, 0);
        QVERIFY(!page.reset);
        QVERIFY(page.live);
        QVERIFY(!page.events.isEmpty());
        QVERIFY(!QJsonDocument(page.events).toJson().contains("Renamed once"));
        const auto event = page.events.last().toObject();
        const auto content = event.value("content").toObject();
        const auto local = first.last().toObject();
        const auto plaintext = device.decrypt(content, Access::aad(cloud, local, content),
                                              machineKey, "session", 1, &error);
        QVERIFY2(!plaintext.isEmpty(), qPrintable(error));
        QVERIFY(Access::peerEvents(cloud, "session", 2, 0).reset);
        peers.closeAll();
    }
    void largeItemsReachThePeerWithTheirChunks() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite"));
        StoredSession session;
        session.id = "session";
        session.worktreePath = dir.path();
        session.state = "ready";
        session.adapter = "codex";
        QVERIFY(journal.upsertSession(session));
        CodexAdapter provider(dir.filePath("never-run-provider"));
        SessionHost host(&journal, &provider);
        r03::Transport network;
        CloudControlClient cloud(&journal, &host);
        Access::configure(cloud, network);
        CredentialStore deviceStore("peer-device");
        CloudCrypto device(&deviceStore);
        const QJsonObject recipient{
            {"id", "device"}, {"keyVersion", 1}, {"keys", device.publicKeys()}};
        QVERIFY(!Access::crypto(cloud)
                     .wrapSessionKey("session", 1, 1, recipient, "machine-synthetic", 1)
                     .isEmpty());
        QString error;
        const QString text(600 * 1024, 'x');
        QVERIFY2(journal.appendEvent("session", 1, "user.message", {{"text", "write it"}}, &error) >
                     0,
                 qPrintable(error));
        QVERIFY2(
            journal.appendEvent(
                "session", 1, "item.completed",
                {{"item_id", "large"},
                 {"item_type", "agentMessage"},
                 {"item", QJsonObject{{"id", "large"}, {"type", "agentMessage"}, {"text", text}}}},
                &error) > 0,
            qPrintable(error));
        QVERIFY2(journal.appendEvent("session", 1, "turn.completed", {{"status", "completed"}},
                                     &error) > 0,
                 qPrintable(error));
        QList<qint64> journaled;
        int chunks = 0;
        for (qint64 after = 0;;) {
            const auto events = journal.eventsAfter("session", 1, after);
            if (events.isEmpty())
                break;
            for (const auto& value : events) {
                after = value.toObject().value("cursor").toInteger();
                journaled.append(after);
                chunks += value.toObject().value("event_type") == "transcript.large_chunk";
            }
        }
        QVERIFY(chunks > 1);

        // Read as a peer reads: page after page from where the last one ended.
        QList<qint64> sent;
        int pages = 0;
        for (qint64 cursor = 0;;) {
            const auto page = Access::peerEvents(cloud, "session", 1, cursor);
            QVERIFY(!page.reset);
            for (const auto& value : page.events)
                sent.append(value.toObject().value("cursor").toInteger());
            cursor = page.nextCursor;
            ++pages;
            if (page.live)
                break;
            QVERIFY(pages < 50);
        }
        QCOMPARE(sent, journaled);
        QVERIFY(pages > 1); // The item does not fit one page; it continues on the next.
    }
};
QTEST_GUILESS_MAIN(PeerTransportTest)
#include "PeerTransportTest.moc"
