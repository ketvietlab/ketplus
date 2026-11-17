// ketplus_peer_link on its own: a real in-process libdatachannel pair, offerer against answerer,
// with the signals passed directly instead of through a relay. No session host, network or TURN.
#include "transport/FrameCodec.h"
#include "transport/SendQueue.h"
#include "transport/WebRtcPeerLink.h"

#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QtEndian>
#include <QtTest>
#include <functional>
#include <memory>

using namespace ketplus::daemon::transport;

namespace {
QtMessageHandler previousHandler = nullptr;
const QString wrongFingerprint =
    QStringLiteral("sha-256 00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:"
                   "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF");

struct Inbox {
    QList<Packet> packets;
    FrameCodec codec;
    QList<QJsonObject> frames;
    QStringList errors;
    void attach(WebRtcPeerLink& link) {
        QObject::connect(&link, &PeerLink::received, &link,
                         [this](Channel channel, const QByteArray& bytes, bool binary) {
                             packets.append({channel, bytes, binary});
                             QString error;
                             const auto frame =
                                 codec.receive({channel, bytes, binary},
                                               QDateTime::currentMSecsSinceEpoch(), &error);
                             if (!error.isEmpty())
                                 errors.append(error);
                             else if (!frame.isEmpty())
                                 frames.append(frame);
                         });
    }
};

// Wires two links as the relay would, optionally rewriting the answer before the offerer sees it.
struct Pair {
    WebRtcPeerLink offerer;
    WebRtcPeerLink answerer;
    Inbox offererInbox, answererInbox;
    QString expected; // empty: the answerer's real fingerprint
    std::function<QString(QString)> rewriteAnswer = [](QString sdp) { return sdp; };
    bool answerAccepted{false};
    bool answerRefused{false};

    Pair() {
        offererInbox.attach(offerer);
        answererInbox.attach(answerer);
        QObject::connect(&offerer, &WebRtcPeerLink::localDescription, &answerer,
                         [this](const QString& sdp) { answerer.answer(sdp, {}); });
        QObject::connect(
            &answerer, &WebRtcPeerLink::localDescription, &offerer, [this](const QString& sdp) {
                const auto print = expected.isEmpty() ? answerer.localFingerprint() : expected;
                if (offerer.acceptAnswer(rewriteAnswer(sdp), print))
                    answerAccepted = true;
                else
                    answerRefused = true;
            });
        QObject::connect(
            &offerer, &WebRtcPeerLink::localCandidate, &answerer,
            [this](const QString& c, const QString& mid) { answerer.candidate(c, mid); });
        QObject::connect(
            &answerer, &WebRtcPeerLink::localCandidate, &offerer,
            [this](const QString& c, const QString& mid) { offerer.candidate(c, mid); });
    }
};

QJsonObject frameOfSize(qsizetype size) {
    QJsonObject frame{{"type", "blob"}, {"payload", QString()}};
    const auto overhead = QJsonDocument(frame).toJson(QJsonDocument::Compact).size();
    frame["payload"] = QString(size - overhead, 'x');
    return frame;
}

// Pushes packets through the production send queue, refilling on writable().
void pump(WebRtcPeerLink& link, SendQueue& queue) {
    QObject::connect(&link, &PeerLink::writable, &link, [&] { queue.flush(link); });
    queue.flush(link);
}
} // namespace

class PeerLinkTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        // Each opened native link logs its selected path (covered by the daemon peer suite);
        // how many lines a run logs depends on ICE timing, so drop only those.
        previousHandler = qInstallMessageHandler(
            [](QtMsgType type, const QMessageLogContext& context, const QString& message) {
                if (type == QtInfoMsg && message.startsWith(QLatin1String("ice-path ")))
                    return;
                previousHandler(type, context, message);
            });
    }

    void fingerprintsAreNormalizedAndWeakDigestsRefused() {
        const auto lower = wrongFingerprint.toLower();
        QCOMPARE(WebRtcPeerLink::normalizedFingerprint(lower), wrongFingerprint);
        QCOMPARE(WebRtcPeerLink::normalizedFingerprint("  " + wrongFingerprint + " "),
                 wrongFingerprint);
        QVERIFY(WebRtcPeerLink::normalizedFingerprint("sha-1 00:11:22:33:44:55:66:77:88:99:"
                                                      "AA:BB:CC:DD:EE:FF:00:11:22:33")
                    .isEmpty());
        QVERIFY(WebRtcPeerLink::normalizedFingerprint("sha-256 00:11").isEmpty());
        QVERIFY(WebRtcPeerLink::normalizedFingerprint(wrongFingerprint.left(20)).isEmpty());
        QVERIFY(WebRtcPeerLink::normalizedFingerprint({}).isEmpty());
    }

    void roleIsFixedByTheFirstCall() {
        QTest::failOnWarning(QRegularExpression(".*"));
        WebRtcPeerLink link;
        QVERIFY(!link.acceptAnswer("v=0", wrongFingerprint)); // no offer yet: ignored, not closed
        QVERIFY(link.offer({}));
        QVERIFY(!link.offer({}));
        QVERIFY(!link.answer("v=0", {}));
        QVERIFY(!link.send({Channel::Control, "{\"type\":\"ping\"}", false}));
        link.close();
    }

    void offererAndAnswererCarryFramesOnEveryChannelBothWays() {
        Pair pair;
        QSignalSpy offererOpened(&pair.offerer, &PeerLink::opened);
        QSignalSpy answererOpened(&pair.answerer, &PeerLink::opened);
        QVERIFY(pair.offerer.offer({}));
        QTRY_VERIFY_WITH_TIMEOUT(offererOpened.count() == 1 && answererOpened.count() == 1, 10'000);
        QVERIFY(pair.answerAccepted);
        QVERIFY(!pair.offerer.localFingerprint().isEmpty());
        QCOMPARE(WebRtcPeerLink::normalizedFingerprint(pair.offerer.localFingerprint()),
                 pair.offerer.localFingerprint());
        QVERIFY(pair.offerer.localFingerprint() != pair.answerer.localFingerprint());

        for (const auto channel : {Channel::Control, Channel::Events, Channel::Bulk}) {
            const auto down = QJsonObject{{"type", "down"}, {"channel", int(channel)}};
            const auto up = QJsonObject{{"type", "up"}, {"channel", int(channel)}};
            QVERIFY(pair.offerer.send(FrameCodec::encode(down, channel, 1).first()));
            QVERIFY(pair.answerer.send(FrameCodec::encode(up, channel, 1).first()));
        }
        QTRY_COMPARE_WITH_TIMEOUT(pair.answererInbox.packets.size(), 3, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(pair.offererInbox.packets.size(), 3, 5000);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(pair.answererInbox.packets.at(i).channel, Channel(i));
            QCOMPARE(pair.answererInbox.frames.at(i).value("type"), "down");
            QCOMPARE(pair.answererInbox.frames.at(i).value("channel").toInt(), i);
            QCOMPARE(pair.offererInbox.packets.at(i).channel, Channel(i));
            QCOMPARE(pair.offererInbox.frames.at(i).value("type"), "up");
            QCOMPARE(pair.offererInbox.frames.at(i).value("channel").toInt(), i);
        }

        // A transfer of exactly the current limit passes in both directions.
        const auto largest = frameOfSize(FrameCodec::transferLimit);
        SendQueue offererQueue, answererQueue;
        QVERIFY(offererQueue.append(FrameCodec::encode(largest, Channel::Bulk, 7)));
        QVERIFY(answererQueue.append(FrameCodec::encode(largest, Channel::Bulk, 8)));
        pump(pair.offerer, offererQueue);
        pump(pair.answerer, answererQueue);
        QTRY_COMPARE_WITH_TIMEOUT(pair.answererInbox.frames.size(), 4, 30'000);
        QTRY_COMPARE_WITH_TIMEOUT(pair.offererInbox.frames.size(), 4, 30'000);
        QCOMPARE(pair.answererInbox.frames.last(), largest);
        QCOMPARE(pair.offererInbox.frames.last(), largest);
        QVERIFY(pair.answererInbox.errors.isEmpty());
        QVERIFY(pair.offererInbox.errors.isEmpty());

        // One byte more: the sender's codec refuses it, and a receiver refuses a peer that
        // streams it anyway, at the part that crosses the limit.
        QVERIFY(FrameCodec::encode(frameOfSize(FrameCodec::transferLimit + 1), Channel::Bulk, 9)
                    .isEmpty());
        const auto oversized = QJsonDocument(frameOfSize(FrameCodec::transferLimit + 1))
                                   .toJson(QJsonDocument::Compact);
        constexpr qsizetype partSize = FrameCodec::messageLimit - 12;
        QList<Packet> forged;
        quint32 index = 0;
        for (qsizetype offset = 0; offset < oversized.size(); offset += partSize, ++index) {
            QByteArray packet(12, '\0');
            qToBigEndian(quint64(9), packet.data());
            const bool last = offset + partSize >= oversized.size();
            qToBigEndian(index | (last ? 0x80000000U : 0U), packet.data() + 8);
            packet.append(oversized.mid(offset, partSize));
            forged.append({Channel::Bulk, packet, true});
        }
        SendQueue forgedQueue;
        QVERIFY(forgedQueue.append(forged));
        QObject::disconnect(&pair.offerer, &PeerLink::writable, nullptr, nullptr);
        pump(pair.offerer, forgedQueue);
        QTRY_VERIFY_WITH_TIMEOUT(pair.answererInbox.errors.contains("invalid_transfer_sequence"),
                                 30'000);
        QCOMPARE(pair.answererInbox.frames.size(), 4);
        pair.offerer.close();
        pair.answerer.close();
    }

    void aWrongExpectedFingerprintRefusesTheAnswer() {
        Pair pair;
        pair.expected = wrongFingerprint;
        QSignalSpy closed(&pair.offerer, &PeerLink::closed);
        QSignalSpy opened(&pair.offerer, &PeerLink::opened);
        QVERIFY(pair.offerer.offer({}));
        QTRY_VERIFY_WITH_TIMEOUT(pair.answerRefused, 10'000);
        QCOMPARE(closed.count(), 1);
        QVERIFY(!pair.offerer.send({Channel::Control, "{\"type\":\"ping\"}", false}));
        QTest::qWait(500);
        QCOMPARE(opened.count(), 0);
        QVERIFY(pair.offererInbox.packets.isEmpty());
        QVERIFY(pair.answererInbox.packets.isEmpty());
        pair.answerer.close();
    }

    void aCertificateThatDoesNotMatchTheExpectedFingerprintNeverOpens() {
        // The relay rewrites the answer's fingerprint to the one the caller expects; the
        // answerer's actual certificate then fails DTLS, so nothing passes either way.
        Pair pair;
        pair.expected = wrongFingerprint;
        pair.rewriteAnswer = [](QString sdp) {
            return sdp.replace(QRegularExpression("a=fingerprint:[^\r\n]+"),
                               "a=fingerprint:" + wrongFingerprint);
        };
        QSignalSpy closed(&pair.offerer, &PeerLink::closed);
        QSignalSpy opened(&pair.offerer, &PeerLink::opened);
        connect(&pair.answerer, &PeerLink::opened, &pair.answerer,
                [&] { pair.answerer.send({Channel::Control, "{\"type\":\"welcome\"}", false}); });
        QVERIFY(pair.offerer.offer({}));
        QTRY_VERIFY_WITH_TIMEOUT(pair.answerAccepted, 10'000);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 20'000);
        QCOMPARE(opened.count(), 0);
        QVERIFY(!pair.offerer.send({Channel::Control, "{\"type\":\"ping\"}", false}));
        QVERIFY(pair.offererInbox.packets.isEmpty());
        QVERIFY(pair.answererInbox.packets.isEmpty());
        pair.answerer.close();
    }

    void closingWhileNegotiatingIsSafe() {
        {
            // Before the offer is even described.
            WebRtcPeerLink link;
            QSignalSpy closed(&link, &PeerLink::closed);
            QVERIFY(link.offer({}));
            link.close();
            link.close();
            QCOMPARE(closed.count(), 1);
            QVERIFY(!link.acceptAnswer("v=0", wrongFingerprint));
            QVERIFY(!link.candidate("candidate:1 1 UDP 1 127.0.0.1 9 typ host", "0"));
            QTest::qWait(200);
            QCOMPARE(closed.count(), 1);
        }
        {
            // After the answer, during ICE and DTLS, from either side.
            for (const bool offererCloses : {true, false}) {
                Pair pair;
                QSignalSpy offererOpened(&pair.offerer, &PeerLink::opened);
                QSignalSpy offererClosed(&pair.offerer, &PeerLink::closed);
                // Runs right after the offerer applied the answer, before any native event.
                connect(&pair.answerer, &WebRtcPeerLink::localDescription, &pair.answerer,
                        [&] { (offererCloses ? pair.offerer : pair.answerer).close(); });
                QVERIFY(pair.offerer.offer({}));
                QTRY_VERIFY_WITH_TIMEOUT(pair.answerAccepted, 10'000);
                QTest::qWait(500);
                QCOMPARE(offererOpened.count(), 0);
                QVERIFY(pair.offererInbox.packets.isEmpty());
                QVERIFY(pair.answererInbox.packets.isEmpty());
                if (offererCloses)
                    QCOMPARE(offererClosed.count(), 1);
            }
        }
        {
            // Destroyed with native callbacks still queued.
            auto* offerer = new WebRtcPeerLink;
            auto* answerer = new WebRtcPeerLink;
            connect(offerer, &WebRtcPeerLink::localDescription, answerer,
                    [answerer](const QString& sdp) { answerer->answer(sdp, {}); });
            connect(answerer, &WebRtcPeerLink::localDescription, offerer,
                    [offerer, answerer](const QString& sdp) {
                        offerer->acceptAnswer(sdp, answerer->localFingerprint());
                        delete offerer;
                    });
            QPointer<WebRtcPeerLink> watch(offerer);
            QVERIFY(offerer->offer({}));
            QTRY_VERIFY_WITH_TIMEOUT(watch.isNull(), 10'000);
            delete answerer;
            QTest::qWait(300);
        }
    }

    void closingDuringATransferIsSafe() {
        auto pair = std::make_unique<Pair>();
        QSignalSpy offererOpened(&pair->offerer, &PeerLink::opened);
        QSignalSpy answererOpened(&pair->answerer, &PeerLink::opened);
        QSignalSpy answererClosed(&pair->answerer, &PeerLink::closed);
        QVERIFY(pair->offerer.offer({}));
        QTRY_VERIFY_WITH_TIMEOUT(offererOpened.count() == 1 && answererOpened.count() == 1, 10'000);
        SendQueue queue;
        QVERIFY(queue.append(
            FrameCodec::encode(frameOfSize(FrameCodec::transferLimit), Channel::Bulk, 3)));
        pump(pair->offerer, queue);
        QTRY_VERIFY_WITH_TIMEOUT(pair->answererInbox.packets.size() > 4, 10'000);
        QVERIFY(!queue.empty());
        pair->offerer.close();
        QVERIFY(!pair->offerer.send({Channel::Control, "{\"type\":\"ping\"}", false}));
        QTRY_COMPARE_WITH_TIMEOUT(answererClosed.count(), 1, 10'000);
        QVERIFY(pair->answererInbox.frames.isEmpty());
        // Destroy both while the answerer may still hold queued deliveries.
        pair.reset();
        QTest::qWait(300);
    }
};

QTEST_GUILESS_MAIN(PeerLinkTest)
#include "PeerLinkTest.moc"
