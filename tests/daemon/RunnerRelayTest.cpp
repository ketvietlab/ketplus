// The daemon's runner routes against a fake Relay on loopback: the route seam, each desktop
// operation with its refusals, the capability gate, schema checks and runner trust.
#include "RunnerTestFixture.h"
#include "runner/CloudRunnerRelay.h"
#include "runner/RunnerPinStore.h"

#include <QStandardPaths>
#include <QtTest>

using namespace runner_test;
using ketplus::daemon::runner::RelayFailure;
using ketplus::daemon::runner::RelayReply;

namespace {

QJsonObject summary(const QString& id, bool paired, const QString& os = "linux") {
    return {{"runnerId", id},
            {"name", "build-node"},
            {"os", os},
            {"arch", "x86_64"},
            {"labels", QJsonArray{"linux"}},
            {"state", "idle"},
            {"runnerVersion", "1.0.0"},
            {"lastSeenAt", QJsonValue()},
            {"pairedWithThisMachine", paired}};
}

QJsonObject page(const QJsonArray& runners, const QJsonValue& next = QJsonValue()) {
    return {{"runners", runners}, {"nextCursor", next}};
}

void pin(const FakeRunner& runner) {
    CredentialStore store;
    runner::RunnerPinStore pins(store);
    const pairing::DesktopRunnerPin record{runner.id,
                                           runner.keyVersion,
                                           runner.key.publicKey,
                                           pairing::runnerFingerprint(runner.key.publicKey),
                                           runner.name,
                                           QDateTime::currentDateTimeUtc()};
    if (!pins.store(record))
        qFatal("Synthetic runner pin failed");
}

} // namespace

class RunnerRelayTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void init() { r03::resetCredentials(); }

    void seamAcceptsRunnerRoutesOnly_data() {
        QTest::addColumn<QByteArray>("method");
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("create") << QByteArray("POST") << "/v2/runner-pairing/tickets" << true;
        QTest::newRow("status") << QByteArray("GET") << "/v2/runner-pairing/tickets/t-1" << true;
        QTest::newRow("approve") << QByteArray("POST") << "/v2/runner-pairing/tickets/t-1/approve"
                                 << true;
        QTest::newRow("reject") << QByteArray("POST") << "/v2/runner-pairing/tickets/t-1/reject"
                                << true;
        QTest::newRow("list") << QByteArray("GET") << "/v2/runners" << true;
        QTest::newRow("list page")
            << QByteArray("GET") << "/v2/runners?limit=100&cursor=c_1" << true;
        for (const char* action : {"unpair", "disable", "enable", "remove"})
            QTest::newRow(action) << QByteArray("POST")
                                  << QStringLiteral("/v2/runners/r-1/%1").arg(action) << true;
        QTest::newRow("heartbeat") << QByteArray("POST") << "/v2/daemon/heartbeat" << false;
        QTest::newRow("commands") << QByteArray("GET") << "/v2/commands:poll?limit=20" << false;
        QTest::newRow("device pairing") << QByteArray("POST") << "/v2/pairing/tickets" << false;
        QTest::newRow("runner register") << QByteArray("POST") << "/v2/runners:register" << false;
        QTest::newRow("runner challenge")
            << QByteArray("POST") << "/v2/runners/challenges" << false;
        QTest::newRow("runner claim")
            << QByteArray("POST") << "/v2/runner/pairing/tickets/t-1/claim" << false;
        QTest::newRow("job submit") << QByteArray("POST") << "/v2/runner-jobs" << true;
        QTest::newRow("job get") << QByteArray("GET") << "/v2/runner-jobs/j-1" << true;
        QTest::newRow("job cancel") << QByteArray("POST") << "/v2/runner-jobs/j-1/cancel" << true;
        QTest::newRow("job list") << QByteArray("GET") << "/v2/runner-jobs" << false;
        QTest::newRow("job submit query") << QByteArray("POST") << "/v2/runner-jobs?x=1" << false;
        QTest::newRow("job get by POST") << QByteArray("POST") << "/v2/runner-jobs/j-1" << false;
        QTest::newRow("job cancel by GET")
            << QByteArray("GET") << "/v2/runner-jobs/j-1/cancel" << false;
        QTest::newRow("job other action")
            << QByteArray("POST") << "/v2/runner-jobs/j-1/finish" << false;
        QTest::newRow("runner leases")
            << QByteArray("POST") << "/v2/runner/leases/l-1/start" << false;
        QTest::newRow("runner acquire") << QByteArray("POST") << "/v2/runner/jobs:acquire" << false;
        QTest::newRow("status by POST")
            << QByteArray("POST") << "/v2/runner-pairing/tickets/t-1" << false;
        QTest::newRow("approve by GET")
            << QByteArray("GET") << "/v2/runner-pairing/tickets/t-1/approve" << false;
        QTest::newRow("list by POST") << QByteArray("POST") << "/v2/runners" << false;
        QTest::newRow("disable by GET") << QByteArray("GET") << "/v2/runners/r-1/disable" << false;
        QTest::newRow("traversal")
            << QByteArray("GET") << "/v2/runner-pairing/tickets/../../x" << false;
        QTest::newRow("encoded id") << QByteArray("POST") << "/v2/runners/r%2F1/remove" << false;
        QTest::newRow("other query") << QByteArray("GET") << "/v2/runners?state=idle" << false;
        QTest::newRow("query on ticket")
            << QByteArray("GET") << "/v2/runner-pairing/tickets/t-1?x=1" << false;
        QTest::newRow("repeated query")
            << QByteArray("GET") << "/v2/runners?limit=1&limit=2" << false;
    }
    void seamAcceptsRunnerRoutesOnly() {
        QFETCH(QByteArray, method);
        QFETCH(QString, path);
        QFETCH(bool, accepted);
        QCOMPARE(runner::isRunnerRoute(method, path), accepted);
        if (accepted)
            return;
        // A refused route sends nothing, also with a valid machine credential.
        Fixture f;
        std::optional<RelayReply> reply;
        f.service().port().runnerRequest(method, path, QJsonObject{},
                                         [&](const RelayReply& value) { reply = value; });
        QTRY_VERIFY(reply.has_value());
        QCOMPARE(reply->failure, RelayFailure::RouteRefused);
        QTest::qWait(50);
        QVERIFY(f.relay.requests.isEmpty());
    }

    void operationsUseContractRoutes() {
        Fixture f;
        const QJsonObject created = f.createTicket({{"runner_id", "runner-a"}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(created.value("error_code").toString()));
        QCOMPARE(f.relay.count(createRoute), 1);
        const auto create = f.relay.of(createRoute).first();
        QCOMPARE(create.json(), (QJsonObject{{"runnerId", "runner-a"}}));
        QCOMPARE(create.authorization, QByteArray("Bearer synthetic-test-token"));

        f.relay.answer(rejectRoute, 204);
        QVERIFY(f.call("runner.pairing.reject", {{"ticket_id", ticketId}}).value("ok").toBool());
        QCOMPARE(f.relay.count(rejectRoute), 1);
        QVERIFY(f.relay.of(rejectRoute).first().body.isEmpty());

        f.relay.answer(listRoute, 200, page({summary("runner-a", true)}));
        const QJsonObject listed = f.call("runner.list");
        QVERIFY(listed.value("ok").toBool());
        QCOMPARE(f.relay.of(listRoute).first().path, QStringLiteral("/v2/runners?limit=100"));
        QCOMPARE(listed.value("runners").toArray().first().toObject().value("runner_id"),
                 QStringLiteral("runner-a"));

        const QList<std::pair<QString, QString>> changes{{"runner.disable", "disabled"},
                                                         {"runner.enable", "idle"},
                                                         {"runner.remove", "removed"}};
        for (const auto& [type, state] : changes) {
            const QString action = type.mid(7);
            const QString route = QStringLiteral("POST /v2/runners/runner-a/") + action;
            f.relay.answer(route, 200, QJsonObject{{"runnerId", "runner-a"}, {"state", state}});
            const QJsonObject result = f.call(type, {{"runner_id", "runner-a"}});
            QVERIFY2(result.value("ok").toBool(), qPrintable(type));
            QCOMPARE(result.value("state").toString(), state);
            QCOMPARE(f.relay.count(route), 1);
        }
        f.relay.answer("POST /v2/runners/runner-a/unpair", 204);
        QVERIFY(f.call("runner.unpair", {{"runner_id", "runner-a"}}).value("ok").toBool());
        QCOMPARE(f.relay.count("POST /v2/runners/runner-a/unpair"), 1);
        // Every request carried the machine credential and named a runner route.
        for (const auto& request : f.relay.requests) {
            QCOMPARE(request.authorization, QByteArray("Bearer synthetic-test-token"));
            QVERIFY(runner::isRunnerRoute(request.method, request.path));
        }
    }

    void refusalsMapToTheirCodes_data() {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("route");
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("expected");
        const QList<std::tuple<QString, QString, QStringList>> operations{
            {"runner.pairing.create", createRoute, {"runner_not_found", "runner_removed"}},
            {"runner.list", listRoute, {}},
            {"runner.disable", "POST /v2/runners/runner-a/disable", {"runner_not_found"}},
            {"runner.enable", "POST /v2/runners/runner-a/enable", {"runner_not_found"}},
            {"runner.remove", "POST /v2/runners/runner-a/remove", {}},
            {"runner.unpair", "POST /v2/runners/runner-a/unpair", {}},
        };
        const QHash<QString, int> statuses{{"entitlement_required", 403}, {"unsupported", 403},
                                           {"invalid_access", 401},       {"rate_limited", 429},
                                           {"invalid_request", 422},      {"runner_not_found", 404},
                                           {"runner_removed", 403},       {"not_paired", 403}};
        for (const auto& [type, route, specific] : operations) {
            QStringList codes{"entitlement_required", "unsupported", "invalid_access",
                              "rate_limited", "invalid_request"};
            codes += specific;
            for (const QString& code : codes)
                QTest::addRow("%s %s", qPrintable(type), qPrintable(code))
                    << type << route << statuses.value(code) << code << code;
            // A code the operation does not name, a server error and no answer at all.
            QTest::addRow("%s foreign", qPrintable(type))
                << type << route << 409 << "job_started" << "relay_error";
            QTest::addRow("%s 500", qPrintable(type))
                << type << route << 500 << "" << "relay_error";
            QTest::addRow("%s network", qPrintable(type)) << type << route << 0 << "" << "network";
        }
    }
    void refusalsMapToTheirCodes() {
        QFETCH(QString, type);
        QFETCH(QString, route);
        QFETCH(int, status);
        QFETCH(QString, code);
        QFETCH(QString, expected);
        Fixture f;
        if (code.isEmpty())
            f.relay.answer(route, status);
        else
            f.relay.refuse(route, status, code);
        const QJsonObject result = f.call(type, {{"runner_id", "runner-a"}});
        QCOMPARE(result.value("ok").toBool(true), false);
        QCOMPARE(result.value("error_code").toString(), expected);
        QVERIFY(!result.value("error_message").toString().isEmpty());
        QVERIFY(!result.value("error_message").toString().contains("synthetic refusal"));
        // Qt resends an idempotent GET once when its connection closes unanswered.
        const int sent = f.relay.count(route);
        QVERIFY(sent == 1 || (expected == "network" && route.startsWith("GET") && sent == 2));
    }

    void ticketRefusalsMapToTheirCodes_data() {
        QTest::addColumn<QString>("route");
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("code");
        QTest::newRow("reject ticket_not_found") << rejectRoute << 404 << "ticket_not_found";
        QTest::newRow("reject rate_limited") << rejectRoute << 429 << "rate_limited";
        QTest::newRow("reject entitlement") << rejectRoute << 403 << "entitlement_required";
        QTest::newRow("approve ticket_not_found") << approveRoute << 404 << "ticket_not_found";
        QTest::newRow("approve ticket_expired") << approveRoute << 410 << "ticket_expired";
        QTest::newRow("approve ticket_used") << approveRoute << 409 << "ticket_used";
        QTest::newRow("approve digest_mismatch") << approveRoute << 409 << "digest_mismatch";
        QTest::newRow("approve invalid_access") << approveRoute << 401 << "invalid_access";
        QTest::newRow("approve unsupported") << approveRoute << 403 << "unsupported";
    }
    void ticketRefusalsMapToTheirCodes() {
        QFETCH(QString, route);
        QFETCH(int, status);
        QFETCH(QString, code);
        Fixture f;
        FakeRunner runner;
        const QJsonObject claimed = f.claimed(runner);
        f.relay.refuse(route, status, code);
        const QJsonObject result =
            route == rejectRoute
                ? f.call("runner.pairing.reject", {{"ticket_id", ticketId}})
                : f.call("runner.pairing.approve",
                         {{"ticket_id", ticketId}, {"code", claimed.value("confirmation_code")}});
        QCOMPARE(result.value("error_code").toString(), code);
        QCOMPARE(f.relay.count(route), 1);
        QVERIFY(f.storedPins().isEmpty());
    }

    void capabilityGatesEveryOperation_data() {
        QTest::addColumn<QString>("type");
        for (const char* type :
             {"runner.pairing.create", "runner.pairing.status", "runner.pairing.approve",
              "runner.pairing.reject", "runner.list", "runner.disable", "runner.enable",
              "runner.remove", "runner.unpair"})
            QTest::newRow(type) << QString::fromLatin1(type);
    }
    void capabilityGatesEveryOperation() {
        QFETCH(QString, type);
        Fixture f;
        f.offerRunners(false);
        pin(FakeRunner{});
        const QJsonObject result =
            f.call(type, {{"runner_id", "runner-a"}, {"ticket_id", ticketId}, {"code", "x"}});
        QCOMPARE(result.value("error_code").toString(), QStringLiteral("runner_unavailable"));
        QTest::qWait(50);
        QVERIFY(f.relay.requests.isEmpty());
        // A gated remove leaves the pin: nothing ran.
        QVERIFY(!f.storedPins().isEmpty());
        // The next heartbeat that offers runners opens the gate.
        f.offerRunners(true);
        f.relay.answer(listRoute, 200, page({}));
        QVERIFY(f.call("runner.list").value("ok").toBool());
    }

    void schemaViolationsAreRefused_data() {
        QTest::addColumn<QJsonObject>("answer");
        QTest::newRow("other os") << page({summary("runner-a", true, "windows")});
        QJsonObject badLabel = summary("runner-a", true);
        badLabel.insert("labels", QJsonArray{"Linux"});
        QTest::newRow("label case") << page({badLabel});
        QJsonObject missing = summary("runner-a", true);
        missing.remove("runnerVersion");
        QTest::newRow("missing field") << page({missing});
        QJsonObject state = summary("runner-a", true);
        state.insert("state", "flying");
        QTest::newRow("unknown state") << page({state});
        QJsonObject id = summary("runner/../a", true);
        QTest::newRow("bad id") << page({id});
        QTest::newRow("duplicate") << page({summary("runner-a", true), summary("runner-a", false)});
        QTest::newRow("no cursor") << QJsonObject{{"runners", QJsonArray{}}};
        QTest::newRow("not an object")
            << QJsonObject{{"runners", "all"}, {"nextCursor", QJsonValue()}};
    }
    void schemaViolationsAreRefused() {
        QFETCH(QJsonObject, answer);
        Fixture f;
        f.relay.answer(listRoute, 200, answer);
        const QJsonObject result = f.call("runner.list");
        QCOMPARE(result.value("error_code").toString(), QStringLiteral("invalid_response"));
        // The daemon goes on answering.
        f.relay.answer(listRoute, 200, page({summary("runner-a", true)}));
        QVERIFY(f.call("runner.list").value("ok").toBool());
    }

    void invalidTicketAnswersAreRefused() {
        Fixture f;
        f.relay.answer(createRoute, 201,
                       QJsonObject{{"ticketId", ticketId},
                                   {"ticketSecret", "short"},
                                   {"expiresAt", "2026-11-18T10:00:00Z"}});
        QCOMPARE(f.call("runner.pairing.create").value("error_code").toString(),
                 QStringLiteral("invalid_response"));
        f.relay.answer(createRoute, 200, QJsonObject{});
        QCOMPARE(f.call("runner.pairing.create").value("error_code").toString(),
                 QStringLiteral("invalid_response"));
        QVERIFY(f.events.isEmpty());
    }

    void listsUntrustedRunnersWithoutPins() {
        Fixture f;
        FakeRunner pinned;
        pin(pinned);
        f.relay.answer(listRoute, 200,
                       page({summary("runner-a", true), summary("runner-b", true),
                             summary("runner-c", false)}));
        const QJsonArray runners = f.call("runner.list").value("runners").toArray();
        QCOMPARE(runners.size(), 3);
        QHash<QString, QJsonObject> byId;
        for (const auto& entry : runners)
            byId.insert(entry.toObject().value("runner_id").toString(), entry.toObject());
        QVERIFY(byId["runner-a"].value("trusted").toBool());
        // Paired with this machine by the Relay's word, but never pinned here.
        QVERIFY(byId["runner-b"].value("paired_with_this_machine").toBool());
        QVERIFY(!byId["runner-b"].value("trusted").toBool());
        QVERIFY(!byId["runner-c"].value("trusted").toBool());
    }

    void listFollowsCursors() {
        Fixture f;
        f.relay.queue(listRoute, 200, page({summary("runner-a", false)}, "cursor_2"));
        f.relay.queue(listRoute, 200, page({summary("runner-b", false)}));
        const QJsonObject result = f.call("runner.list");
        QCOMPARE(result.value("runners").toArray().size(), 2);
        QVERIFY(result.value("complete").toBool());
        QCOMPARE(f.relay.requests.last().path,
                 QStringLiteral("/v2/runners?limit=100&cursor=cursor_2"));
    }

    void removeAndUnpairDropThePinFirst_data() {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("route");
        QTest::addColumn<QString>("doneCode");
        QTest::addColumn<int>("doneStatus");
        QTest::newRow("remove") << "runner.remove" << "POST /v2/runners/runner-a/remove"
                                << "runner_not_found" << 404;
        QTest::newRow("unpair") << "runner.unpair" << "POST /v2/runners/runner-a/unpair"
                                << "not_paired" << 403;
    }
    void removeAndUnpairDropThePinFirst() {
        QFETCH(QString, type);
        QFETCH(QString, route);
        QFETCH(QString, doneCode);
        QFETCH(int, doneStatus);
        Fixture f;
        FakeRunner runner;
        pin(runner);
        f.relay.answer(route, 0); // the Relay cannot be reached
        const QJsonObject failed = f.call(type, {{"runner_id", runner.id}});
        QCOMPARE(failed.value("error_code").toString(), QStringLiteral("network"));
        QCOMPARE(f.relay.count(route), 1);
        // Trust ended before the call: the pin is gone although the Relay never answered.
        QVERIFY(!CredentialStore().read("runner-pins").contains(runner.id.toUtf8()));
        CredentialStore store;
        QVERIFY(!runner::RunnerPinStore(store).pin(runner.id));
        // The retry completes: the Relay already did it.
        f.relay.refuse(route, doneStatus, doneCode);
        const QJsonObject retried = f.call(type, {{"runner_id", runner.id}});
        QVERIFY2(retried.value("ok").toBool(), qPrintable(retried.value("error_code").toString()));
        QCOMPARE(f.relay.count(route), 2);
    }

    void pinStoreFailureStopsRemoval() {
        Fixture f;
        FakeRunner runner;
        pin(runner);
        r03::failWrites = true;
        const QJsonObject result = f.call("runner.unpair", {{"runner_id", runner.id}});
        QCOMPARE(result.value("error_code").toString(), QStringLiteral("pin_store_failed"));
        QVERIFY(f.relay.requests.isEmpty());
    }

    void disableAndEnableFollowTheRelay() {
        Fixture f;
        const QString route = "POST /v2/runners/runner-a/disable";
        f.relay.answer(route, 0);
        QCOMPARE(f.call("runner.disable", {{"runner_id", "runner-a"}}).value("error_code"),
                 QJsonValue("network"));
        f.relay.refuse(route, 404, "runner_not_found");
        QCOMPARE(f.call("runner.disable", {{"runner_id", "runner-a"}}).value("error_code"),
                 QJsonValue("runner_not_found"));
        // An answer for another runner, or another state, is not believed.
        f.relay.answer(route, 200, QJsonObject{{"runnerId", "runner-b"}, {"state", "disabled"}});
        QCOMPARE(f.call("runner.disable", {{"runner_id", "runner-a"}}).value("error_code"),
                 QJsonValue("invalid_response"));
        f.relay.answer(route, 200, QJsonObject{{"runnerId", "runner-a"}, {"state", "disabled"}});
        const QJsonObject done = f.call("runner.disable", {{"runner_id", "runner-a"}});
        QVERIFY(done.value("ok").toBool());
        QCOMPARE(done.value("state").toString(), QStringLiteral("disabled"));
        QCOMPARE(f.call("runner.disable", {{"runner_id", "../x"}}).value("error_code"),
                 QJsonValue("invalid_request"));
    }
};

QTEST_GUILESS_MAIN(RunnerRelayTest)
#include "RunnerRelayTest.moc"
