#include "agents/AgentMutationOrigin.h"
#include <QtTest>

using namespace ketplus::daemon;
class AgentMutationOriginTest final : public QObject {
    Q_OBJECT
  private slots:
    void pendingSpoofedAndMissingOriginsFailClosed() {
        AgentMutationOrigin authority;
        const auto first = authority.prepare("session", 1, 8, "full", false);
        QVERIFY(!first.credential.isEmpty());
        QVERIFY(!authority.authorize(first.credential, "session", 1));
        QVERIFY(!authority.activate("spoofed epoch"));
        QVERIFY(authority.activate(first.transportEpoch));
        QVERIFY(!authority.authorize({}, "session", 1));
        QVERIFY(!authority.authorize("session-read-token", "session", 1));
        QVERIFY(!authority.authorize(first.credential, "another-session", 1));
        QVERIFY(!authority.authorize(first.credential, "session", 2));
        QVERIFY(authority.authorize(first.credential, "session", 1));
    }

    void delayedInvocationNeverBorrowsNewTurnOrReviewAuthority_data() {
        QTest::addColumn<QString>("provider");
        for (const auto* provider : {"claude", "codex", "cursor", "copilot"})
            QTest::newRow(provider) << provider;
    }
    void delayedInvocationNeverBorrowsNewTurnOrReviewAuthority() {
        QFETCH(QString, provider);
        AgentMutationOrigin authority;
        const auto old = authority.prepare(provider, 1, 1, "ask", false);
        QVERIFY(authority.activate(old.transportEpoch));
        const auto accepted = authority.authorize(old.credential, provider, 1);
        QVERIFY(accepted);
        authority.revoke(); // idle/drain, including an invocation queued before revoke
        QVERIFY(!authority.authorize(old.credential, provider, 1));
        for (const auto* profile : {"read", "verify"}) {
            Q_UNUSED(profile);
            const auto review = authority.prepare(provider, 1, 2, "full", true);
            QVERIFY(review.credential.isEmpty());
            QVERIFY(authority.activate(review.transportEpoch));
            QVERIFY(!authority.authorize({}, provider, 1));
            QVERIFY(!authority.authorize(old.credential, provider, 1));
            const auto next = authority.prepare(provider, 1, 3, "full", false);
            QVERIFY(!authority.authorize(next.credential, provider, 1));
            QVERIFY(!authority.activate(old.transportEpoch));
            QVERIFY(authority.activate(next.transportEpoch));
            QVERIFY(!authority.authorize(review.credential, provider, 1));
            QVERIFY(!authority.authorize(old.credential, provider, 1));
            const auto normal = authority.authorize(next.credential, provider, 1);
            QVERIFY(normal);
            QCOMPARE(normal->turn, 3);
            QCOMPARE(normal->executionPolicy, "full");
            // An accepted child/process retains its original policy despite changes.
            QCOMPARE(accepted->turn, 1);
            QCOMPARE(accepted->executionPolicy, "ask");
        }
    }

    void reconnectEpochInvalidatesOldAndDuplicateInvocationsStayBound() {
        AgentMutationOrigin authority;
        const auto first = authority.prepare("session", 1, 5, "ask", false);
        QVERIFY(authority.activate(first.transportEpoch));
        const auto one = authority.authorize(first.credential, "session", 1);
        const auto duplicate = authority.authorize(first.credential, "session", 1);
        QVERIFY(one && duplicate);
        QCOMPARE(one->transportEpoch, duplicate->transportEpoch);
        const auto reconnect = authority.prepare("session", 1, 5, "ask", false);
        QVERIFY(reconnect.transportEpoch != first.transportEpoch);
        QVERIFY(reconnect.credential != first.credential);
        QVERIFY(!authority.activate(first.transportEpoch));
        QVERIFY(!authority.authorize(first.credential, "session", 1));
        QVERIFY(authority.activate(reconnect.transportEpoch));
        QVERIFY(authority.authorize(reconnect.credential, "session", 1));
        authority.prepare("session", 2, 1, "invalid-policy", false);
        QVERIFY(!authority.authorize(reconnect.credential, "session", 1));
        QVERIFY(authority.bootstrap().credential.isEmpty());
    }
};
QTEST_GUILESS_MAIN(AgentMutationOriginTest)
#include "AgentMutationOriginTest.moc"
