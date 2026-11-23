// The desktop's frame codec and per-conversation event cursor, against scripted frames and an
// in-memory cursor store: no socket, no network.
#include "control/RelayEventFeed.h"
#include "cloud-privacy/MemoryCredentials.h"
#include "cloud/CredentialStore.h"
#include "control/RelayFrames.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace ketplus::remote;
namespace {
QJsonObject sampleEvent(qint64 cursor, qint64 generation = 1) {
    return {{"cursor", cursor}, {"session_generation", generation}, {"event_type", "user.message"}};
}
QByteArray page(const QString& session, std::initializer_list<qint64> cursors, bool live = false,
                qint64 generation = 1) {
    QJsonArray events;
    for (const qint64 cursor : cursors)
        events.append(sampleEvent(cursor, generation));
    return QJsonDocument(
               QJsonObject{
                   {"type", "events"}, {"sessionId", session}, {"events", events}, {"live", live}})
        .toJson(QJsonDocument::Compact);
}
QByteArray live(const QString& session, qint64 cursor, qint64 generation = 1) {
    return QJsonDocument(QJsonObject{{"type", "event"},
                                     {"sessionId", session},
                                     {"event", sampleEvent(cursor, generation)}})
        .toJson(QJsonDocument::Compact);
}
QByteArray simple(const QString& type, const QString& session = {}, qint64 after = -1,
                  qint64 generation = 0) {
    QJsonObject o{{"type", type}};
    if (!session.isEmpty())
        o.insert("sessionId", session);
    if (after >= 0)
        o.insert("afterCursor", after);
    if (generation > 0)
        o.insert("sessionGeneration", generation);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
QJsonObject parse(const QByteArray& bytes) { return QJsonDocument::fromJson(bytes).object(); }
QList<qint64> cursors(const QJsonArray& events) {
    QList<qint64> out;
    for (const auto& e : events)
        out.append(e.toObject().value("cursor").toInteger());
    return out;
}

class MemoryCursors final : public EventCursorStore {
  public:
    QHash<QString, qint64> values;
    int saves{0};
    bool failing{false};
    qint64 load(const QString& s, qint64 g) const override {
        return values.value(QStringLiteral("%1:%2").arg(s).arg(g));
    }
    bool save(const QList<Entry>& entries) override {
        if (failing)
            return false;
        ++saves;
        for (const auto& e : entries)
            values.insert(QStringLiteral("%1:%2").arg(e.sessionId).arg(e.generation), e.cursor);
        return true;
    }
};
} // namespace

class RelayEventFeedTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { r03::resetCredentials(); }

    void subscribeAndUnsubscribeStayUnderTheLimit() {
        QString error;
        const auto bytes = frames::encodeSubscribe("session-a", 3, 42, &error);
        QVERIFY2(!bytes.isEmpty(), qPrintable(error));
        QVERIFY(bytes.size() <= frames::outboundLimit);
        const auto o = parse(bytes);
        QCOMPARE(o.value("type").toString(), QStringLiteral("subscribe"));
        QCOMPARE(o.value("sessionId").toString(), QStringLiteral("session-a"));
        QCOMPARE(o.value("sessionGeneration").toInteger(), 3);
        QCOMPARE(o.value("afterCursor").toInteger(), 42);
        QVERIFY(!frames::encodeUnsubscribe("session-a").isEmpty());
        QVERIFY(frames::encodeSubscribe(QString(300, 'x'), 1, 0, &error).isEmpty());
        QVERIFY(frames::encodeSubscribe("session-a", 0, 0).isEmpty());
        QVERIFY(frames::encodeSubscribe("session-a", 1, -1).isEmpty());
        QVERIFY(frames::encodeUnsubscribe({}).isEmpty());
    }

    void decodeRefusesUnknownOversizeAndMalformedFrames_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("unknown type") << simple("welcome", "s");
        QTest::newRow("not json") << QByteArray("{nope");
        QTest::newRow("not an object") << QByteArray("[1]");
        QTest::newRow("oversize") << QByteArray(frames::inboundLimit + 1, ' ');
        QTest::newRow("gap without cursor") << simple("gap", "s");
        QTest::newRow("gap without session") << simple("gap", {}, 3);
        QTest::newRow("reset without generation") << simple("session.reset", "s");
        QTest::newRow("events without array") << QByteArray(R"({"type":"events","sessionId":"s"})");
        QTest::newRow("cursors not increasing") << page("s", {3, 3});
        QTest::newRow("cursor missing")
            << QByteArray(R"({"type":"events","sessionId":"s","events":[{"x":1}]})");
        QTest::newRow("fractional cursor")
            << QByteArray(R"({"type":"events","sessionId":"s","events":[{"cursor":1.5}]})");
        QTest::newRow("generations differ") << QByteArray(
            R"({"type":"events","sessionId":"s","events":[)"
            R"({"cursor":1,"session_generation":1},{"cursor":2,"session_generation":2}]})");
        QTest::newRow("event frame without event")
            << QByteArray(R"({"type":"event","sessionId":"s"})");
    }
    void decodeRefusesUnknownOversizeAndMalformedFrames() {
        QFETCH(QByteArray, bytes);
        QString error;
        QVERIFY(!frames::decode(bytes, &error));
        QVERIFY(!error.isEmpty());
    }

    void pageOver100EventsIsRefused() {
        QJsonArray events;
        for (int i = 1; i <= frames::pageEventLimit + 1; ++i)
            events.append(sampleEvent(i));
        const auto bytes =
            QJsonDocument(QJsonObject{{"type", "events"}, {"sessionId", "s"}, {"events", events}})
                .toJson(QJsonDocument::Compact);
        QVERIFY(!frames::decode(bytes));
    }

    void connectingSubscribesAfterTheStoredCursorOfEachConversation() {
        MemoryCursors store;
        store.values.insert("a:1", 7);
        RelayEventFeed feed(&store);
        QVERIFY(feed.follow("a", 1).isEmpty()); // no connection yet
        QVERIFY(feed.follow("b", 2).isEmpty());
        const auto sent = feed.connected();
        QCOMPARE(sent.size(), 2);
        QHash<QString, qint64> after;
        for (const auto& frame : sent)
            after.insert(parse(frame).value("sessionId").toString(),
                         parse(frame).value("afterCursor").toInteger());
        QCOMPARE(after.value("a"), 7);
        QCOMPARE(after.value("b"), 0);
        // Every new connection subscribes again after the cursor reached meanwhile.
        QVERIFY(feed.receive(page("a", {8, 9}, true)).live);
        feed.disconnected();
        for (const auto& frame : feed.connected())
            if (parse(frame).value("sessionId") == "a")
                QCOMPARE(parse(frame).value("afterCursor").toInteger(), 9);
    }

    void deliversEachEventOnceInOrder() {
        MemoryCursors store;
        RelayEventFeed feed(&store);
        (void)feed.connected();
        (void)feed.follow("a", 1);
        auto r = feed.receive(page("a", {1, 2, 3}));
        QCOMPARE(cursors(r.events), (QList<qint64>{1, 2, 3}));
        // A repeated page, a late event and an older page deliver nothing and never lower the
        // cursor.
        QVERIFY(feed.receive(page("a", {1, 2, 3})).events.isEmpty());
        QVERIFY(feed.receive(live("a", 2)).events.isEmpty());
        QCOMPARE(feed.cursor("a"), 3);
        // A page that overlaps the cursor delivers only its new events.
        QCOMPARE(cursors(feed.receive(page("a", {2, 3, 4, 5})).events), (QList<qint64>{4, 5}));
        QCOMPARE(cursors(feed.receive(live("a", 6)).events), (QList<qint64>{6}));
        QCOMPARE(feed.cursor("a"), 6);
    }

    void gapSubscribesAgainAfterTheCursorAndLosesNothing() {
        MemoryCursors store;
        RelayEventFeed feed(&store);
        (void)feed.connected();
        (void)feed.follow("a", 1);
        (void)feed.receive(page("a", {1, 2}, true));
        const auto gap = feed.receive(simple("gap", "a", 5));
        QCOMPARE(gap.send.size(), 1);
        QCOMPARE(parse(gap.send.first()).value("afterCursor").toInteger(), 2);
        // The host re-sends from the cursor; events already delivered are not delivered again.
        QCOMPARE(cursors(feed.receive(page("a", {3, 4, 5}, true)).events),
                 (QList<qint64>{3, 4, 5}));
        // A gap of a conversation no longer followed does nothing.
        QVERIFY(feed.receive(simple("gap", "other", 0)).send.isEmpty());
    }

    void framesDroppedSubscribesEveryFollowedConversationAgain() {
        MemoryCursors store;
        RelayEventFeed feed(&store);
        (void)feed.connected();
        (void)feed.follow("a", 1);
        (void)feed.follow("b", 1);
        (void)feed.receive(page("a", {1, 2}));
        (void)feed.receive(page("b", {1}));
        auto r = feed.receive(simple("frames.dropped"));
        QCOMPARE(r.send.size(), 2);
        QHash<QString, qint64> after;
        for (const auto& frame : r.send)
            after.insert(parse(frame).value("sessionId").toString(),
                         parse(frame).value("afterCursor").toInteger());
        QCOMPARE(after.value("a"), 2);
        QCOMPARE(after.value("b"), 1);
        // One that names a conversation resubscribes only that one.
        r = feed.receive(simple("frames.dropped", "b"));
        QCOMPARE(r.send.size(), 1);
        QCOMPARE(parse(r.send.first()).value("sessionId").toString(), QStringLiteral("b"));
    }

    void resetReloadsTheWindowAndIgnoresEventsUntilAdopted() {
        MemoryCursors store;
        RelayEventFeed feed(&store);
        (void)feed.connected();
        (void)feed.follow("a", 1);
        (void)feed.receive(page("a", {1, 2, 3}));
        const auto reset = feed.receive(simple("session.reset", "a", -1, 1));
        QVERIFY(reset.reload);
        QVERIFY(feed.reloading("a"));
        // Events, gaps and drops of a conversation being reloaded would mix old and new history.
        QVERIFY(feed.receive(page("a", {4, 5})).events.isEmpty());
        QVERIFY(feed.receive(simple("gap", "a", 3)).send.isEmpty());
        QVERIFY(feed.receive(simple("frames.dropped")).send.isEmpty());
        // A reconnect does not subscribe it either; the window comes first.
        feed.disconnected();
        QVERIFY(feed.connected().isEmpty());
        const auto subscribe = feed.adoptWindow("a", 1, 40);
        QCOMPARE(parse(subscribe).value("afterCursor").toInteger(), 40);
        QVERIFY(!feed.reloading("a"));
        QCOMPARE(store.values.value("a:1"), 40);
        // Events at or below the snapshot are in the window already.
        QCOMPARE(cursors(feed.receive(page("a", {39, 40, 41})).events), (QList<qint64>{41}));
    }

    void aNewGenerationIsReloadedAndOldFramesAreIgnored() {
        MemoryCursors store;
        RelayEventFeed feed(&store);
        (void)feed.connected();
        (void)feed.follow("a", 1);
        (void)feed.receive(page("a", {1, 2}));
        QVERIFY(feed.receive(simple("session.reset", "a", -1, 2)).reload);
        (void)feed.adoptWindow("a", 2, 10);
        QVERIFY(feed.receive(page("a", {11, 12}, false, 1)).events.isEmpty()); // an old generation
        QCOMPARE(cursors(feed.receive(page("a", {11, 12}, false, 2)).events),
                 (QList<qint64>{11, 12}));
        // A reset of an older generation than the one followed is ignored.
        QVERIFY(!feed.receive(simple("session.reset", "a", -1, 1)).reload);
    }

    void cursorSurvivesARestartThroughTheStore() {
        MemoryCursors store;
        {
            RelayEventFeed feed(&store);
            (void)feed.connected();
            (void)feed.follow("a", 1);
            (void)feed.receive(page("a", {1, 2, 3}));
            // A few live events stay unsaved until the interval, a page, unfollow or destruction.
            (void)feed.receive(live("a", 4));
            QCOMPARE(store.values.value("a:1"), 3);
        }
        QCOMPARE(store.values.value("a:1"), 4);
        RelayEventFeed again(&store);
        const auto subscribe = again.follow("a", 1);
        QVERIFY(subscribe.isEmpty());
        QCOMPARE(parse(again.connected().first()).value("afterCursor").toInteger(), 4);
    }

    void liveEventsAreSavedEveryFiftyNotEachOne() {
        MemoryCursors store;
        RelayEventFeed feed(&store);
        (void)feed.connected();
        (void)feed.follow("a", 1);
        for (int i = 1; i <= RelayEventFeed::liveSaveInterval - 1; ++i)
            (void)feed.receive(live("a", i));
        QCOMPARE(store.saves, 0);
        (void)feed.receive(live("a", RelayEventFeed::liveSaveInterval));
        QCOMPARE(store.saves, 1);
        QCOMPARE(store.values.value("a:1"), RelayEventFeed::liveSaveInterval);
        // A page saves at once, in one write for all conversations that moved.
        (void)feed.follow("b", 1);
        (void)feed.receive(page("b", {1, 2}));
        QCOMPARE(store.saves, 2);
    }

    void aFailedSaveKeepsTheCursorDirtyAndLosesNoEvent() {
        MemoryCursors store;
        RelayEventFeed feed(&store);
        (void)feed.connected();
        (void)feed.follow("a", 1);
        store.failing = true;
        QCOMPARE(cursors(feed.receive(page("a", {1, 2})).events), (QList<qint64>{1, 2}));
        QVERIFY(!feed.flush());
        store.failing = false;
        QVERIFY(feed.flush());
        QCOMPARE(store.values.value("a:1"), 2);
    }

    void rejectedFramesChangeNothing() {
        MemoryCursors store;
        RelayEventFeed feed(&store);
        (void)feed.connected();
        (void)feed.follow("a", 1);
        (void)feed.receive(page("a", {1, 2}));
        QString error;
        const auto r = feed.receive(page("a", {5, 4}), &error);
        QVERIFY(!error.isEmpty());
        QVERIFY(r.events.isEmpty() && r.send.isEmpty() && !r.reload);
        QCOMPARE(feed.cursor("a"), 2);
        // The error says why and never carries event content.
        QVERIFY(!error.contains("user.message"));
    }

    void unfollowSendsUnsubscribeAndLateFramesAreIgnored() {
        MemoryCursors store;
        RelayEventFeed feed(&store);
        (void)feed.connected();
        (void)feed.follow("a", 1);
        (void)feed.receive(live("a", 1));
        const auto bye = feed.unfollow("a");
        QCOMPARE(parse(bye).value("type").toString(), QStringLiteral("unsubscribe"));
        QCOMPARE(store.values.value("a:1"), 1);
        QVERIFY(feed.receive(page("a", {2})).events.isEmpty());
        QVERIFY(feed.unfollow("a").isEmpty());
    }

    void credentialCursorsKeepNoConversationIdInTheAccountName() {
        ketplus::daemon::CredentialStore credentials("ketplus-test");
        CredentialEventCursors cursors(&credentials);
        QVERIFY(cursors.save({{"session-secret-name", 2, 9}, {"other", 1, 4}}));
        QCOMPARE(cursors.load("session-secret-name", 2), 9);
        QCOMPARE(cursors.load("session-secret-name", 1), 0);
        QCOMPARE(cursors.load("missing", 1), 0);
        QCOMPARE(r03::rewrites, 1); // both in one commit
        for (auto it = r03::credentials.cbegin(); it != r03::credentials.cend(); ++it) {
            QVERIFY(!it.key().contains("session-secret-name"));
            QVERIFY(!it.value().contains("session-secret-name"));
        }
        // A new process reads what the first one saved.
        CredentialEventCursors again(&credentials);
        QCOMPARE(again.load("session-secret-name", 2), 9);
        QCOMPARE(again.load("other", 1), 4);
    }

    void storeAccessDoesNotGrowWithTheNumberOfConversations_data() {
        QTest::addColumn<int>("conversations");
        QTest::newRow("1") << 1;
        QTest::newRow("40") << 40;
    }
    void storeAccessDoesNotGrowWithTheNumberOfConversations() {
        QFETCH(int, conversations);
        ketplus::daemon::CredentialStore credentials("ketplus-test");
        CredentialEventCursors cursors(&credentials);
        RelayEventFeed feed(&cursors);
        (void)feed.connected();
        for (int i = 0; i < conversations; ++i)
            (void)feed.follow(QStringLiteral("session-%1").arg(i), 1);
        // Opening the feed reads the store once, whatever the number of conversations.
        QCOMPARE(cursors.storeReads(), 1);
        for (int i = 0; i < conversations; ++i)
            (void)feed.receive(page(QStringLiteral("session-%1").arg(i), {1, 2}));
        // Each page saves every cursor that changed in one write: pages of one conversation
        // each, so as many writes as pages, never more than one per page.
        QCOMPARE(r03::rewrites, conversations);
        // A flush of many changed cursors is a single commit.
        r03::rewrites = 0;
        for (int i = 0; i < conversations; ++i)
            (void)feed.receive(live(QStringLiteral("session-%1").arg(i), 3));
        QCOMPARE(r03::rewrites, 0);
        QVERIFY(feed.flush());
        QCOMPARE(r03::rewrites, 1);
        QCOMPARE(cursors.storeReads(), 1);
    }

    void eventsAfterALostSaveAreRedeliveredAndDeduplicatedByCursor() {
        MemoryCursors store;
        QSet<qint64> consumer; // an integrator keeps events by cursor
        const auto take = [&consumer](const QJsonArray& events) {
            for (const auto cursor : cursors(events))
                consumer.insert(cursor);
        };
        {
            RelayEventFeed feed(&store);
            (void)feed.connected();
            (void)feed.follow("a", 1);
            take(feed.receive(page("a", {1, 2, 3})).events);
            store.failing = true; // the app is lost before the next save
            take(feed.receive(live("a", 4)).events);
            take(feed.receive(live("a", 5)).events);
        }
        QCOMPARE(store.values.value("a:1"), 3);
        store.failing = false;
        RelayEventFeed again(&store);
        (void)again.follow("a", 1);
        QCOMPARE(parse(again.connected().first()).value("afterCursor").toInteger(), 3);
        // The host sends from the saved cursor: 4 and 5 arrive again, once each.
        const auto redelivered = again.receive(page("a", {4, 5, 6}, true)).events;
        QCOMPARE(cursors(redelivered), (QList<qint64>{4, 5, 6}));
        take(redelivered);
        QCOMPARE(consumer, (QSet<qint64>{1, 2, 3, 4, 5, 6}));
    }

    void cursorsOutsideTheExactIntegerRangeAreRefused_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("huge cursor")
            << QByteArray(R"({"type":"events","sessionId":"s","events":[{"cursor":1e300}]})");
        QTest::newRow("cursor above 2^53") << QByteArray(
            R"({"type":"events","sessionId":"s","events":[{"cursor":9007199254740994}]})");
        QTest::newRow("negative huge cursor")
            << QByteArray(R"({"type":"events","sessionId":"s","events":[{"cursor":-1e300}]})");
        QTest::newRow("huge generation")
            << QByteArray(R"({"type":"session.reset","sessionId":"s","sessionGeneration":1e300})");
        QTest::newRow("huge gap cursor")
            << QByteArray(R"({"type":"gap","sessionId":"s","afterCursor":1e19})");
        QTest::newRow("huge event generation")
            << QByteArray(R"({"type":"events","sessionId":"s","events":[)"
                          R"({"cursor":1,"session_generation":1e300}]})");
    }
    void cursorsOutsideTheExactIntegerRangeAreRefused() {
        QFETCH(QByteArray, bytes);
        QString error;
        QVERIFY(!frames::decode(bytes, &error));
        QVERIFY(!error.isEmpty());
    }

    void cursorsAtTheExactLimitAreAccepted() {
        const auto frame =
            frames::decode(R"({"type":"gap","sessionId":"s","afterCursor":9007199254740992})");
        QVERIFY(frame);
        QCOMPARE(frame->afterCursor, 9007199254740992);
    }
};

QTEST_GUILESS_MAIN(RelayEventFeedTest)
#include "RelayEventFeedTest.moc"
