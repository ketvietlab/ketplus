#include "runtime/ProviderCatalogCache.h"
#include <QJsonArray>
#include <QtTest>
using namespace ketplus::daemon;
class ProviderCatalogCacheTest : public QObject {
    Q_OBJECT
    static QJsonObject catalog(const QString& id) {
        return {{"catalog", QJsonArray{QJsonObject{{"id", id}}}}};
    }
  private slots:
    void coldRequestsCoalesceAndProvidersDoNotBlockEachOther() {
        ProviderCatalogCache cache;
        ProviderCatalogCache::Callback codex, claude;
        int calls = 0, replies = 0, otherReplies = 0;
        for (int i = 0; i < 12; ++i)
            cache.get(
                "codex",
                [&](auto done) {
                    ++calls;
                    codex = done;
                },
                [&](auto value, auto error) {
                    QVERIFY(error.isEmpty());
                    QCOMPARE(value, catalog("codex"));
                    ++replies;
                });
        cache.get("claude", [&](auto done) { claude = done; }, [&](auto, auto) { ++otherReplies; });
        QTRY_COMPARE(calls, 1);
        QTRY_VERIFY(bool(claude));
        codex(catalog("codex"), {});
        QCOMPARE(replies, 12);
        QCOMPARE(otherReplies, 0);
        cache.get(
            "codex", [&](auto) { QFAIL("Fresh cache must not fetch"); },
            [&](auto value, auto error) {
                QCOMPARE(value, catalog("codex"));
                QVERIFY(error.isEmpty());
                ++replies;
            });
        QCOMPARE(replies, 13);
        claude(catalog("claude"), {});
        QCOMPARE(otherReplies, 1);
    }
    void staleReturnsImmediatelyAndFailureKeepsLastGoodCatalog() {
        ProviderCatalogCache cache(nullptr, 0, 1000, 1000);
        ProviderCatalogCache::Callback refresh;
        cache.get("codex", [&](auto done) { done(catalog("good"), {}); }, [](auto, auto) {});
        QTest::qWait(1);
        bool immediate = false;
        cache.get(
            "codex", [&](auto done) { refresh = done; },
            [&](auto value, auto error) {
                QCOMPARE(value, catalog("good"));
                QVERIFY(error.isEmpty());
                immediate = true;
            });
        QVERIFY(immediate);
        QTRY_VERIFY(bool(refresh));
        refresh({}, "offline");
        bool stillGood = false;
        cache.get(
            "codex", [](auto) { QFAIL("Backoff must suppress retries"); },
            [&](auto value, auto error) {
                QCOMPARE(value, catalog("good"));
                QVERIFY(error.isEmpty());
                stillGood = true;
            });
        QVERIFY(stillGood);
    }
    void retryWaitersJoinTheNewRequestInsteadOfReceivingThePreviousError() {
        ProviderCatalogCache cache(nullptr, 60000, 1000, 20);
        int errors = 0;
        cache.get(
            "codex", [](auto done) { done({}, "offline"); },
            [&](auto, auto error) {
                QVERIFY(!error.isEmpty());
                ++errors;
            });
        QTRY_COMPARE(errors, 1);
        QTest::qWait(25);
        ProviderCatalogCache::Callback retry;
        int replies = 0;
        auto accept = [&](auto result, auto error) {
            QVERIFY(error.isEmpty());
            QCOMPARE(result, catalog("recovered"));
            ++replies;
        };
        cache.get("codex", [&](auto done) { retry = done; }, accept);
        cache.get("codex", [](auto) { QFAIL("Retry must coalesce"); }, accept);
        QTRY_VERIFY(bool(retry));
        retry(catalog("recovered"), {});
        QCOMPARE(replies, 2);
    }

    void invalidationRejectsLateCompletionAndTimeoutSettlesWaiters() {
        ProviderCatalogCache cache(nullptr, 60000, 20, 0);
        ProviderCatalogCache::Callback old;
        int refused = 0;
        cache.get(
            "codex", [&](auto done) { old = done; },
            [&](auto, auto error) {
                QVERIFY(!error.isEmpty());
                ++refused;
            });
        QTRY_VERIFY(bool(old));
        cache.invalidate("codex");
        QCOMPARE(refused, 1);
        bool fresh = false;
        cache.get(
            "codex", [&](auto done) { done(catalog("new-account"), {}); },
            [&](auto value, auto error) {
                QCOMPARE(value, catalog("new-account"));
                QVERIFY(error.isEmpty());
                fresh = true;
            });
        QTRY_VERIFY(fresh);
        old(catalog("old-account"), {});
        cache.get(
            "codex", [](auto) { QFAIL("Unexpected fetch"); },
            [&](auto value, auto) { QCOMPARE(value, catalog("new-account")); });
        int timedOut = 0;
        cache.get(
            "hung", [](auto) {},
            [&](auto, auto error) {
                QVERIFY(!error.isEmpty());
                ++timedOut;
            });
        QTRY_COMPARE(timedOut, 1);
    }
};
QTEST_GUILESS_MAIN(ProviderCatalogCacheTest)
#include "ProviderCatalogCacheTest.moc"
