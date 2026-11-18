#include "RunnerAppTest.h"

#include "relay/RelayClient.h"
#include "relay/RelayLeaseCalls.h"
#include "run/AccessTokens.h"
#include "state/RunnerState.h"
#include "state/StateDirectory.h"

#include <QJsonArray>
#include <QTest>

namespace runner_app_test {

using namespace ketplus::runner::app;

namespace {

// A registered runner and the job calls of its Relay client, with a token.
struct JobCalls {
    explicit JobCalls(TestHost& host)
        : environment(host.environment()),
          directory(*StateDirectory::open(host.stateDir(), &error)), state(directory),
          identity(*state.loadIdentity(&error)), client(host.relay->url(), 5'000),
          tokens(client, state, identity, environment) {}

    RelayResult call(const std::function<RelayResult(RelayClient&, const QString&)>& request) {
        const AccessTokens::Call result =
            tokens.call([&](const QString& token) { return request(client, token); });
        if (result.token.outcome != AccessTokens::Outcome::Ok)
            return result.token.relay;
        return result.relay;
    }
    RelayResult acquire(const QString& acquireId, const QString& jobId = {}) {
        return call(
            [&](RelayClient& c, const QString& t) { return c.acquireJob(acquireId, jobId, t); });
    }
    RelayResult renew(const QString& leaseId, const QString& phase = QStringLiteral("assigned")) {
        return call(
            [&](RelayClient& c, const QString& t) { return c.renewLease(leaseId, phase, t); });
    }
    RelayResult start(const QString& leaseId) {
        return call([&](RelayClient& c, const QString& t) { return c.startJob(leaseId, t); });
    }
    RelayResult finish(const QString& leaseId, const QString& outcome, const QString& reason) {
        return call([&](RelayClient& c, const QString& t) {
            return c.finishJob(leaseId, outcome, reason, t);
        });
    }
    RelayResult release(const QString& leaseId, const QString& reason) {
        return call(
            [&](RelayClient& c, const QString& t) { return c.releaseLease(leaseId, reason, t); });
    }

    QString error;
    Environment environment;
    StateDirectory directory;
    RunnerState state;
    RunnerIdentity identity;
    RelayClient client;
    AccessTokens tokens;
};

void queueJob(TestHost& host, const QString& id = QStringLiteral("job-1")) {
    FakeRelay::Job job;
    job.id = id;
    job.runsOn = {QStringLiteral("os:linux"), QStringLiteral("gpu")};
    host.relay->jobs.append(job);
}

const FakeRelay::Request& lastRequest(const TestHost& host) { return host.relay->requests.last(); }

} // namespace

void RunnerAppJobCallsTest::acquireReturnsTheLease() {
    TestHost host;
    host.registerRunner();
    queueJob(host);
    JobCalls calls(host);
    const RelayResult lease = calls.acquire(QStringLiteral("acq-1"));
    QVERIFY2(lease.ok(), qPrintable(lease.describe()));
    QCOMPARE(lastRequest(host).path, QStringLiteral("/v2/runner/jobs:acquire"));
    QCOMPARE(lastRequest(host).body, (QJsonObject{{QStringLiteral("acquireId"), "acq-1"}}));
    QCOMPARE(lease.body.value("jobId").toString(), QStringLiteral("job-1"));
    QCOMPARE(lease.body.value("attempt").toInt(), 1);
    QVERIFY(!lease.body.value("leaseId").toString().isEmpty());
    QVERIFY(!lease.body.value("leaseExpiresAt").toString().isEmpty());
    QCOMPARE(lease.body.value("requester").toObject().value("machineId").toString(),
             QStringLiteral("desktop-1"));
    QCOMPARE(lease.body.value("runsOn").toArray().size(), 2);

    // The same acquireId again: the same lease.
    const RelayResult again = calls.acquire(QStringLiteral("acq-1"));
    QVERIFY(again.ok());
    QCOMPARE(again.body.value("leaseId"), lease.body.value("leaseId"));
    // Another one while it is held: runner_busy carries the held lease.
    const RelayResult busy = calls.acquire(QStringLiteral("acq-2"), QStringLiteral("job-1"));
    QVERIFY(busy.refused("runner_busy"));
    QCOMPARE(lastRequest(host).body.value("jobId").toString(), QStringLiteral("job-1"));
    const QJsonObject held = busy.body.value("lease").toObject();
    QCOMPARE(held.value("leaseId"), lease.body.value("leaseId"));
    QCOMPARE(held.value("jobId").toString(), QStringLiteral("job-1"));
    QCOMPARE(held.value("attempt").toInt(), 1);
    QVERIFY(!held.value("leaseExpiresAt").toString().isEmpty());
}

void RunnerAppJobCallsTest::acquireRefusals_data() {
    QTest::addColumn<QString>("code");
    QTest::addColumn<int>("status");
    for (const char* code : {"job_unavailable", "runner_busy", "label_mismatch"})
        QTest::newRow(code) << QString::fromLatin1(code) << 409;
    for (const char* code :
         {"runner_disabled", "not_paired", "entitlement_required", "unsupported"})
        QTest::newRow(code) << QString::fromLatin1(code) << 403;
    QTest::newRow("rate_limited") << QStringLiteral("rate_limited") << 429;
    QTest::newRow("invalid_request") << QStringLiteral("invalid_request") << 422;
}

void RunnerAppJobCallsTest::acquireRefusals() {
    QFETCH(QString, code);
    QFETCH(int, status);
    TestHost host;
    host.registerRunner();
    JobCalls calls(host);
    host.relay->intercept =
        [&](const FakeRelay::Request& request) -> std::optional<FakeRelay::Reply> {
        if (!request.path.startsWith(QStringLiteral("/v2/runner/jobs")))
            return std::nullopt;
        return FakeRelay::Reply{status, {{QStringLiteral("code"), code}}};
    };
    const RelayResult result = calls.acquire(QStringLiteral("acq-1"));
    QVERIFY(result.refused(code.toLatin1().constData()));
    QCOMPARE(result.status, status);
}

void RunnerAppJobCallsTest::leaseCalls() {
    TestHost host;
    host.registerRunner();
    queueJob(host);
    JobCalls calls(host);
    const QString leaseId = calls.acquire(QStringLiteral("acq-1")).body.value("leaseId").toString();

    const RelayResult renewed = calls.renew(leaseId, QStringLiteral("syncing"));
    QVERIFY(renewed.ok());
    QCOMPARE(lastRequest(host).path, QStringLiteral("/v2/runner/leases/%1/renew").arg(leaseId));
    QCOMPARE(lastRequest(host).body, (QJsonObject{{QStringLiteral("phase"), "syncing"}}));
    QVERIFY(!renewed.body.value("leaseExpiresAt").toString().isEmpty());

    const RelayResult started = calls.start(leaseId);
    QVERIFY(started.ok());
    QCOMPARE(lastRequest(host).path, QStringLiteral("/v2/runner/leases/%1/start").arg(leaseId));
    QVERIFY(lastRequest(host).raw.isEmpty());
    host.clock = host.clock.addSecs(5);
    QCOMPARE(calls.start(leaseId).body.value("startedAt"), started.body.value("startedAt"));

    QVERIFY(calls.release(leaseId, QStringLiteral("draining")).refused("job_started"));

    const RelayResult finished =
        calls.finish(leaseId, QStringLiteral("succeeded"), QStringLiteral("exited"));
    QVERIFY(finished.ok());
    QCOMPARE(lastRequest(host).body, (QJsonObject{{QStringLiteral("outcome"), "succeeded"},
                                                  {QStringLiteral("reason"), "exited"}}));
    QCOMPARE(finished.body.value("outcome").toString(), QStringLiteral("succeeded"));
    // The same body again returns the record; another body is lease_closed with it.
    QVERIFY(calls.finish(leaseId, QStringLiteral("succeeded"), QStringLiteral("exited")).ok());
    const RelayResult other =
        calls.finish(leaseId, QStringLiteral("failed"), QStringLiteral("exited_nonzero"));
    QVERIFY(other.refused("lease_closed"));
    QCOMPARE(other.body.value("recorded").toObject().value("outcome").toString(),
             QStringLiteral("succeeded"));
    // A closed or unknown lease: lease_expired.
    QVERIFY(calls.renew(leaseId).refused("lease_expired"));
    QVERIFY(calls.renew(QStringLiteral("lease-unknown")).refused("lease_expired"));
}

void RunnerAppJobCallsTest::releaseBeforeStart() {
    TestHost host;
    host.registerRunner();
    queueJob(host);
    JobCalls calls(host);
    const QString leaseId = calls.acquire(QStringLiteral("acq-1")).body.value("leaseId").toString();
    const RelayResult released = calls.release(leaseId, QStringLiteral("untrusted_requester"));
    QVERIFY(released.ok());
    QCOMPARE(lastRequest(host).path, QStringLiteral("/v2/runner/leases/%1/release").arg(leaseId));
    QCOMPARE(lastRequest(host).body,
             (QJsonObject{{QStringLiteral("reason"), "untrusted_requester"}}));
    QCOMPARE(released.body.value("requeued").toBool(), true);
    QVERIFY(calls.release(leaseId, QStringLiteral("untrusted_requester")).ok()); // repeat
    const RelayResult finish =
        calls.finish(leaseId, QStringLiteral("rejected"), QStringLiteral("spec_invalid"));
    QVERIFY(finish.refused("lease_closed"));
    QCOMPARE(finish.body.value("recorded").toObject().value("outcome").toString(),
             QStringLiteral("released"));
}

void RunnerAppJobCallsTest::relayEndedTheJob() {
    TestHost host;
    host.registerRunner();
    queueJob(host);
    JobCalls calls(host);
    const QString leaseId = calls.acquire(QStringLiteral("acq-1")).body.value("leaseId").toString();
    host.relay->jobs.first().state = QStringLiteral("failed");
    host.relay->jobs.first().reason = QStringLiteral("not_paired");
    for (const RelayResult& result : {calls.renew(leaseId), calls.start(leaseId)}) {
        QVERIFY(result.refused("job_cancelled"));
        QCOMPARE(result.body.value("job").toObject().value("reason").toString(),
                 QStringLiteral("not_paired"));
    }
    for (const RelayResult& result :
         {calls.finish(leaseId, QStringLiteral("interrupted"), QStringLiteral("lease_lost")),
          calls.release(leaseId, QStringLiteral("draining"))}) {
        QVERIFY(result.refused("lease_closed"));
        QCOMPARE(result.body.value("job").toObject().value("state").toString(),
                 QStringLiteral("failed"));
    }
}

void RunnerAppJobCallsTest::lostAnswers_data() {
    QTest::addColumn<QString>("route");
    QTest::addColumn<bool>("drop");
    for (const char* route : {"jobs:acquire", "/renew", "/start", "/finish", "/release"}) {
        QTest::newRow(qPrintable(QStringLiteral("%1 network").arg(route)))
            << QString::fromLatin1(route) << true;
        QTest::newRow(qPrintable(QStringLiteral("%1 5xx").arg(route)))
            << QString::fromLatin1(route) << false;
    }
}

void RunnerAppJobCallsTest::lostAnswers() {
    QFETCH(QString, route);
    QFETCH(bool, drop);
    TestHost host;
    host.registerRunner();
    queueJob(host);
    JobCalls calls(host);
    const QString leaseId =
        route == QStringLiteral("jobs:acquire")
            ? QString()
            : calls.acquire(QStringLiteral("acq-0")).body.value("leaseId").toString();
    host.relay->rewrite = [&](const FakeRelay::Request& request, FakeRelay::Reply& reply) {
        if (!request.path.contains(route))
            return;
        if (drop)
            reply.drop = true; // handled, then no answer
        else
            reply = FakeRelay::Reply{503, {{QStringLiteral("code"), QStringLiteral("unknown")}}};
    };
    RelayResult result;
    if (route == QStringLiteral("jobs:acquire"))
        result = calls.acquire(QStringLiteral("acq-1"));
    else if (route == QStringLiteral("/renew"))
        result = calls.renew(leaseId);
    else if (route == QStringLiteral("/start"))
        result = calls.start(leaseId);
    else if (route == QStringLiteral("/finish"))
        result = calls.finish(leaseId, QStringLiteral("failed"), QStringLiteral("exited_nonzero"));
    else
        result = calls.release(leaseId, QStringLiteral("draining"));
    QVERIFY(result.lostAnswer());
    QCOMPARE(result.kind, drop ? RelayResult::Kind::Network : RelayResult::Kind::ServerError);
}

void RunnerAppJobCallsTest::invalidAccessRenewsTheTokenOnce() {
    TestHost host;
    host.registerRunner();
    queueJob(host);
    JobCalls calls(host);
    int refusals = 0;
    host.relay->intercept =
        [&](const FakeRelay::Request& request) -> std::optional<FakeRelay::Reply> {
        if (request.path.startsWith(QStringLiteral("/v2/runner/jobs")) && refusals++ == 0)
            return FakeRelay::Reply{401,
                                    {{QStringLiteral("code"), QStringLiteral("invalid_access")}}};
        return std::nullopt;
    };
    const int tokensBefore =
        host.relay->count(QStringLiteral("POST"), QStringLiteral("credentials"));
    QVERIFY(calls.acquire(QStringLiteral("acq-1")).ok());
    QCOMPARE(host.relay->count(QStringLiteral("POST"), QStringLiteral("jobs:acquire")), 2);
    // One token for the first call, one renewal after invalid_access.
    QCOMPARE(host.relay->count(QStringLiteral("POST"), QStringLiteral("credentials")),
             tokensBefore + 2);
}

void RunnerAppJobCallsTest::deliveryCallsMapTheAnswers() {
    using ketplus::runner::delivery::CallResult;
    using ketplus::runner::delivery::LeaseCall;
    TestHost host;
    host.registerRunner();
    queueJob(host);
    JobCalls calls(host);
    RelayLeaseCalls delivery(calls.client, calls.tokens, {});
    LeaseCall acquire;
    acquire.kind = LeaseCall::Kind::Acquire;
    acquire.acquireId = QStringLiteral("acq-1");
    const CallResult lease = delivery.call(acquire);
    QCOMPARE(lease.kind, CallResult::Kind::Ok);
    QCOMPARE(lease.body.value("jobId").toString(), QStringLiteral("job-1"));

    acquire.acquireId = QStringLiteral("acq-2");
    const CallResult busy = delivery.call(acquire);
    QVERIFY(busy.refused("runner_busy"));
    QCOMPARE(busy.body.value("lease").toObject().value("leaseId"), lease.body.value("leaseId"));

    LeaseCall renew;
    renew.kind = LeaseCall::Kind::Renew;
    renew.leaseId = lease.body.value("leaseId").toString();
    renew.phase = QStringLiteral("assigned");
    host.relay->rewrite = [](const FakeRelay::Request&, FakeRelay::Reply& reply) {
        reply.drop = true;
    };
    QCOMPARE(delivery.call(renew).kind, CallResult::Kind::Lost);
    host.relay->rewrite = {};

    // invalid_access, a new token refused too: the refresh credential is gone.
    host.relay->intercept =
        [](const FakeRelay::Request& request) -> std::optional<FakeRelay::Reply> {
        if (request.path.contains(QStringLiteral("/renew")) ||
            request.path == QStringLiteral("/v2/runners/credentials"))
            return FakeRelay::Reply{401,
                                    {{QStringLiteral("code"), QStringLiteral("invalid_access")}}};
        return std::nullopt;
    };
    QCOMPARE(delivery.call(renew).kind, CallResult::Kind::CredentialLost);
}

} // namespace runner_app_test
