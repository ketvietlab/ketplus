#include "RunnerTestFixture.h"

#include "runner/jobs/RemoteJobLabels.h"
#include "runner/jobs/RemoteJobRelay.h"

using namespace runner_test;
using namespace ketplus::daemon::runner::jobs;
using ketplus::daemon::runner::runnerFailure;

namespace {

const QString submitRoute = QStringLiteral("POST /v2/runner-jobs");
const QString getRoute = QStringLiteral("GET /v2/runner-jobs/job-1");
const QString cancelRoute = QStringLiteral("POST /v2/runner-jobs/job-1/cancel");

QJsonObject relayJob(const QString& state = QStringLiteral("queued")) {
    return {{"jobId", "job-1"},
            {"state", state},
            {"attempt", 0},
            {"runnerId", QJsonValue()},
            {"queuedAt", "2026-11-18T09:00:00Z"},
            {"expiresAt", "2026-11-19T09:00:00Z"},
            {"startedAt", QJsonValue()},
            {"endedAt", QJsonValue()},
            {"outcome", QJsonValue()},
            {"reason", QJsonValue()}};
}

// Runs one call and waits for its answer.
JobOutcome waitFor(Fixture& fixture,
                   const std::function<void(RemoteJobRelay&, JobCallback)>& call) {
    RemoteJobRelay relay(fixture.service().port());
    std::optional<JobOutcome> outcome;
    call(relay, [&](const JobOutcome& value) { outcome = value; });
    if (!QTest::qWaitFor([&] { return outcome.has_value(); }, 5000))
        return JobOutcome{std::nullopt, runnerFailure(QStringLiteral("test_timeout"))};
    return *outcome;
}

JobOutcome submit(Fixture& f, const QStringList& runsOn = {"os:linux", "gpu"}) {
    return waitFor(f, [&](RemoteJobRelay& relay, JobCallback done) {
        relay.submit(QStringLiteral("task-1"), runsOn, std::move(done));
    });
}

JobOutcome get(Fixture& f) {
    return waitFor(f, [](RemoteJobRelay& relay, JobCallback done) {
        relay.get(QStringLiteral("job-1"), std::move(done));
    });
}

JobOutcome cancel(Fixture& f) {
    return waitFor(f, [](RemoteJobRelay& relay, JobCallback done) {
        relay.cancel(QStringLiteral("job-1"), QStringLiteral("user"), std::move(done));
    });
}

JobOutcome run(Fixture& f, const QString& operation) {
    if (operation == QStringLiteral("submit"))
        return submit(f);
    if (operation == QStringLiteral("get"))
        return get(f);
    return cancel(f);
}

QString routeOf(const QString& operation) {
    if (operation == QStringLiteral("submit"))
        return submitRoute;
    return operation == QStringLiteral("get") ? getRoute : cancelRoute;
}

int statusOf(const QString& operation) { return operation == QStringLiteral("submit") ? 202 : 200; }

} // namespace

class RemoteJobRelayTest final : public QObject {
    Q_OBJECT

  private slots:
    void runsOnFollowsTheAgentSurface_data() {
        QTest::addColumn<QStringList>("labels");
        QTest::addColumn<bool>("valid");
        QStringList sixteen;
        for (int index = 0; index < 16; ++index)
            sixteen << QStringLiteral("label-%1").arg(index);
        QTest::newRow("none") << QStringList{} << false;
        QTest::newRow("one") << QStringList{"os:linux"} << true;
        QTest::newRow("sixteen") << sixteen << true;
        QTest::newRow("seventeen") << (QStringList(sixteen) << "label-16") << false;
        QTest::newRow("system labels")
            << QStringList{"os:linux", "arch:x86_64", "arch:arm64"} << true;
        QTest::newRow("64 characters") << QStringList{QString(64, 'a')} << true;
        QTest::newRow("65 characters") << QStringList{QString(65, 'a')} << false;
        QTest::newRow("user colon") << QStringList{"team:build"} << false;
        QTest::newRow("other os") << QStringList{"os:windows"} << false;
        QTest::newRow("other arch") << QStringList{"arch:riscv64"} << false;
        QTest::newRow("upper case") << QStringList{"GPU"} << false;
        QTest::newRow("upper system") << QStringList{"OS:linux"} << false;
        QTest::newRow("duplicate") << QStringList{"gpu", "gpu"} << false;
        QTest::newRow("empty label") << QStringList{""} << false;
        QTest::newRow("leading dot") << QStringList{".gpu"} << false;
        QTest::newRow("inner punctuation") << QStringList{"gpu.a_b-c"} << true;
        QTest::newRow("space") << QStringList{"gpu "} << false;
    }
    void runsOnFollowsTheAgentSurface() {
        QFETCH(QStringList, labels);
        QFETCH(bool, valid);
        QCOMPARE(isValidRunsOn(labels), valid);
    }

    void callsUseContractRoutesAndFields() {
        Fixture f;
        f.relay.answer(submitRoute, 202, relayJob());
        f.relay.answer(getRoute, 200, relayJob("acquired"));
        f.relay.answer(cancelRoute, 200, relayJob("cancelled"));

        const JobOutcome submitted = submit(f);
        QVERIFY(submitted.ok());
        QCOMPARE(submitted.value->state, QStringLiteral("queued"));
        const QJsonObject body = f.relay.of(submitRoute).last().json();
        QCOMPARE(body.value("submitKey").toString(), QStringLiteral("task-1"));
        QCOMPARE(body.value("runsOn").toArray(), QJsonArray({"os:linux", "gpu"}));
        QCOMPARE(body.size(), 2);

        QCOMPARE(get(f).value->state, QStringLiteral("acquired"));
        QVERIFY(f.relay.of(getRoute).last().body.isEmpty());

        QCOMPARE(cancel(f).value->state, QStringLiteral("cancelled"));
        QCOMPARE(f.relay.of(cancelRoute).last().json(), QJsonObject({{"reason", "user"}}));
        for (const QString& route : {submitRoute, getRoute, cancelRoute})
            QCOMPARE(f.relay.count(route), 1);
    }

    void jobAnswerIsReadWhole() {
        Fixture f;
        QJsonObject job = relayJob("finished");
        job["attempt"] = 2;
        job["runnerId"] = "runner-a";
        job["startedAt"] = "2026-11-18T09:01:00.250Z";
        job["endedAt"] = "2026-11-18T09:05:00Z";
        job["outcome"] = "succeeded";
        job["reason"] = "exit_code";
        job["extra"] = "ignored";
        f.relay.answer(getRoute, 200, job);
        const JobOutcome outcome = get(f);
        QVERIFY(outcome.ok());
        QCOMPARE(outcome.value->attempt, 2);
        QCOMPARE(outcome.value->runnerId.value_or(QString()), QStringLiteral("runner-a"));
        QCOMPARE(outcome.value->startedAt->toMSecsSinceEpoch() % 1000, 250);
        QCOMPARE(outcome.value->outcome.value_or(QString()), QStringLiteral("succeeded"));
        QCOMPARE(outcome.value->reason.value_or(QString()), QStringLiteral("exit_code"));
        QVERIFY(outcome.value->terminal());
    }

    void refusalsMapToTheirCodes_data() {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("expected");
        const QHash<QString, int> statuses{
            {"entitlement_required", 403}, {"unsupported", 403},        {"invalid_access", 401},
            {"rate_limited", 429},         {"invalid_request", 422},    {"job_not_found", 404},
            {"idempotency_conflict", 409}, {"no_matching_runner", 409}, {"queue_full", 429},
            {"invalid_labels", 422}};
        const QList<std::pair<QString, QStringList>> operations{
            {"submit",
             {"idempotency_conflict", "no_matching_runner", "queue_full", "invalid_labels"}},
            {"get", {"job_not_found"}},
            {"cancel", {"job_not_found"}},
        };
        for (const auto& [operation, specific] : operations) {
            QStringList codes{"entitlement_required", "unsupported", "invalid_access",
                              "rate_limited", "invalid_request"};
            codes += specific;
            for (const QString& code : codes)
                QTest::addRow("%s %s", qPrintable(operation), qPrintable(code))
                    << operation << statuses.value(code) << code << code;
            // Codes of the other operations, a server error and no answer at all.
            const QString foreign = operation == "submit" ? "job_not_found" : "queue_full";
            QTest::addRow("%s foreign", qPrintable(operation))
                << operation << 409 << foreign << "relay_error";
            QTest::addRow("%s 500", qPrintable(operation))
                << operation << 500 << "" << "relay_error";
            QTest::addRow("%s 503 with code", qPrintable(operation))
                << operation << 503 << "rate_limited" << "relay_error";
            QTest::addRow("%s network", qPrintable(operation)) << operation << 0 << "" << "network";
        }
    }
    void refusalsMapToTheirCodes() {
        QFETCH(QString, operation);
        QFETCH(int, status);
        QFETCH(QString, code);
        QFETCH(QString, expected);
        Fixture f;
        const QString route = routeOf(operation);
        if (code.isEmpty())
            f.relay.answer(route, status);
        else
            f.relay.refuse(route, status, code);
        const JobOutcome outcome = run(f, operation);
        QVERIFY(!outcome.ok());
        QCOMPARE(outcome.failure.code, expected);
        QVERIFY(!outcome.failure.message.isEmpty());
        QVERIFY(!outcome.failure.message.contains("synthetic refusal"));
        // Qt resends an idempotent GET once when its connection closes unanswered.
        const int sent = f.relay.count(route);
        QVERIFY(sent == 1 || (expected == "network" && operation == "get" && sent == 2));
    }

    void schemaViolationsAreRefused_data() {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        const QList<std::pair<QString, QJsonValue>> breaks{
            {"jobId", "bad/id"},
            {"jobId", "job-2"}, // another job than the one asked for (get, cancel)
            {"state", "running"},
            {"state", QJsonValue()},
            {"attempt", 4},
            {"attempt", -1},
            {"attempt", 1.5},
            {"attempt", "1"},
            {"runnerId", "bad id"},
            {"runnerId", QJsonValue(QJsonValue::Undefined)},
            {"queuedAt", "yesterday"},
            {"queuedAt", "2026-11-18T09:00:00"}, // no zone
            {"queuedAt", QJsonValue()},
            {"expiresAt", QJsonValue(QJsonValue::Undefined)},
            {"startedAt", 5},
            {"endedAt", QJsonValue(QJsonValue::Undefined)},
            {"outcome", "exited"},
            {"outcome", QJsonValue(QJsonValue::Undefined)},
            {"reason", "Free text"},
            {"reason", QString(65, 'a')},
            {"reason", QJsonValue(QJsonValue::Undefined)},
        };
        for (const QString operation : {"submit", "get", "cancel"})
            for (const auto& [field, value] : breaks) {
                if (operation == "submit" && value == QJsonValue("job-2"))
                    continue;
                QTest::addRow(
                    "%s %s %s", qPrintable(operation), qPrintable(field),
                    qPrintable(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)))
                    << operation << field << value;
            }
    }
    void schemaViolationsAreRefused() {
        QFETCH(QString, operation);
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        Fixture f;
        QJsonObject job = relayJob();
        if (value.isUndefined())
            job.remove(field);
        else
            job[field] = value;
        f.relay.answer(routeOf(operation), statusOf(operation), job);
        const JobOutcome outcome = run(f, operation);
        QVERIFY(!outcome.ok());
        QCOMPARE(outcome.failure.code, QStringLiteral("invalid_response"));
    }

    void otherSuccessStatusesAreRefused() {
        Fixture f;
        f.relay.answer(submitRoute, 200, relayJob()); // the contract answers 202
        QCOMPARE(submit(f).failure.code, QStringLiteral("invalid_response"));
        f.relay.answer(getRoute, 202, relayJob());
        QCOMPARE(get(f).failure.code, QStringLiteral("invalid_response"));
        f.relay.answer(cancelRoute, 200, QJsonArray{});
        QCOMPARE(cancel(f).failure.code, QStringLiteral("invalid_response"));
    }

    void invalidInputSendsNothing() {
        Fixture f;
        QCOMPARE(submit(f, {"Upper"}).failure.code, QStringLiteral("invalid_labels"));
        QCOMPARE(submit(f, {}).failure.code, QStringLiteral("invalid_labels"));
        const JobOutcome badKey = waitFor(f, [](RemoteJobRelay& relay, JobCallback done) {
            relay.submit(QStringLiteral("task/1"), {"gpu"}, std::move(done));
        });
        QCOMPARE(badKey.failure.code, QStringLiteral("invalid_request"));
        const JobOutcome badId = waitFor(f, [](RemoteJobRelay& relay, JobCallback done) {
            relay.get(QStringLiteral("../job"), std::move(done));
        });
        QCOMPARE(badId.failure.code, QStringLiteral("job_not_found"));
        const JobOutcome badReason = waitFor(f, [](RemoteJobRelay& relay, JobCallback done) {
            relay.cancel(QStringLiteral("job-1"), QStringLiteral("Stop now"), std::move(done));
        });
        QCOMPARE(badReason.failure.code, QStringLiteral("invalid_request"));
        QVERIFY(f.relay.requests.isEmpty());
    }
};

QTEST_GUILESS_MAIN(RemoteJobRelayTest)
#include "RemoteJobRelayTest.moc"
