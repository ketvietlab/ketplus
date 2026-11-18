#include "RunnerSessionTest.h"
#include "delivery/GuardedAcceptedJobs.h"
#include "delivery/JobDelivery.h"
#include "delivery/JobSessionLink.h"

#include <QJsonArray>
#include <QTest>

#include <memory>

namespace runner_session_test {

using namespace protocol;
using delivery::CallResult;
using delivery::LeaseCall;

namespace {

class Calls final : public delivery::RelayCalls {
  public:
    void send(const LeaseCall& call) override { sent.append(call); }
    QList<LeaseCall> sent;
};

class Journal final : public delivery::DeliveryJournal {
  public:
    [[nodiscard]] bool write(const delivery::JournalRecord&) override { return true; }
    [[nodiscard]] bool clear() override { return true; }
};

class Trust final : public delivery::RequesterTrust {
  public:
    [[nodiscard]] bool trusted(const QString& machineId) const override {
        return machineId == QStringLiteral("desktop-1");
    }
};

class Timers final : public delivery::DeliveryClock {
  public:
    void arm(delivery::DeliveryTimer, qint64) override {}
    void disarm(delivery::DeliveryTimer) override {}
};

QJsonObject leaseBody() {
    return {{QStringLiteral("jobId"), QStringLiteral("job-1")},
            {QStringLiteral("attempt"), 1},
            {QStringLiteral("leaseId"), QStringLiteral("lease-1")},
            {QStringLiteral("leaseExpiresAt"), QStringLiteral("2026-11-18T09:05:00Z")},
            {QStringLiteral("requester"),
             QJsonObject{{QStringLiteral("workspaceId"), QStringLiteral("ws-1")},
                         {QStringLiteral("machineId"), QStringLiteral("desktop-1")}}},
            {QStringLiteral("runsOn"), QJsonArray{QStringLiteral("gpu")}}};
}

// A real session behind a JobDelivery, with fake Relay calls.
struct Rig {
    Rig() {
        delivery = std::make_unique<delivery::JobDelivery>(
            delivery::effectiveLabels({QStringLiteral("gpu")}, QStringLiteral("x86_64")),
            delivery::DeliveryPorts{calls, link, journal, trust, restart, timers,
                                    [] { return 0.0; }, [] { return QStringLiteral("acq-1"); },
                                    [this](const QString& message) { reports.append(message); }});
        guard = std::make_unique<delivery::GuardedAcceptedJobs>(accepted, *delivery);
        host = std::make_unique<Host>(guard.get(), &delivery->lease());
        link.attach(*host->session);
    }

    // The heartbeat says a job waits; the acquire returns a lease from desktop-1.
    void hold() {
        delivery->heartbeatAnswered(true, false);
        answer({CallResult::Kind::Ok, {}, leaseBody()});
    }
    void answer(const CallResult& result) { delivery->answered(calls.sent.last(), result); }
    [[nodiscard]] QStringList kinds() const {
        static const QStringList names{"acquire", "renew", "start", "finish", "release"};
        QStringList out;
        for (const LeaseCall& call : calls.sent)
            out.append(names.at(int(call.kind)));
        return out;
    }
    [[nodiscard]] QString heartbeatState() const {
        return delivery->heartbeatFields().value(QStringLiteral("state")).toString();
    }
    [[nodiscard]] QString heartbeatPhase() const {
        return delivery->heartbeatFields()
            .value(QStringLiteral("currentJob"))
            .toObject()
            .value(QStringLiteral("phase"))
            .toString();
    }

    Calls calls;
    delivery::JobSessionLink link;
    Journal journal;
    Trust trust;
    delivery::NeverRelaunch restart;
    Timers timers;
    QStringList reports;
    FakeAccepted accepted;
    std::unique_ptr<delivery::JobDelivery> delivery;
    std::unique_ptr<delivery::GuardedAcceptedJobs> guard;
    std::unique_ptr<Host> host;
};

} // namespace

void RunnerSessionDeliveryTest::unrecordedSpecStopsTakingJobs() {
    Rig rig;
    rig.accepted.failAdd = true; // the record cannot reach the disk
    rig.hold();
    QCOMPARE(rig.host->session->phase(), session::JobSession::Phase::Assigned);
    rig.host->open();
    rig.host->welcome();
    rig.host->clearSent();
    rig.host->deliver(makeSpec());

    QCOMPARE(rig.host->sentOf<JobRejected>().size(), 1);
    QCOMPARE(rig.host->sentOf<JobRejected>().first().code, QStringLiteral("runner_unavailable"));
    QVERIFY(rig.host->command.launches.isEmpty());
    QCOMPARE(rig.kinds(), (QStringList{"acquire", "finish"}));
    QCOMPARE(rig.calls.sent.last().outcome, QStringLiteral("rejected"));
    QCOMPARE(rig.calls.sent.last().reason, QStringLiteral("runner_unavailable"));
    QCOMPARE(rig.reports.size(), 1);

    rig.answer({CallResult::Kind::Ok, {}, {}});
    QCOMPARE(rig.delivery->state(), delivery::JobDelivery::State::Idle);
    QCOMPARE(rig.heartbeatState(), QStringLiteral("draining"));
    rig.delivery->heartbeatAnswered(true, false);
    rig.delivery->jobAvailable(QStringLiteral("job-2"));
    QCOMPARE(rig.kinds(), (QStringList{"acquire", "finish"}));
    QCOMPARE(rig.reports.size(), 1);
}

void RunnerSessionDeliveryTest::sessionPhasesReachTheHeartbeat() {
    Rig rig;
    rig.hold();
    QCOMPARE(rig.heartbeatState(), QStringLiteral("busy"));
    QCOMPARE(rig.heartbeatPhase(), QStringLiteral("assigned"));
    rig.host->open();
    rig.host->welcome();
    rig.host->deliver(makeSpec());
    QCOMPARE(rig.kinds(), (QStringList{"acquire", "start"}));
    QCOMPARE(rig.heartbeatPhase(), QStringLiteral("assigned"));

    rig.answer({CallResult::Kind::Ok, {}, {{QStringLiteral("startedAt"), startedAt}}});
    QCOMPARE(rig.host->session->phase(), session::JobSession::Phase::Syncing);
    QCOMPARE(rig.heartbeatPhase(), QStringLiteral("syncing"));

    rig.host->session->snapshotReady(QStringLiteral("/work/ws"), {});
    QCOMPARE(rig.host->command.launches.size(), 1);
    QCOMPARE(rig.heartbeatPhase(), QStringLiteral("running"));
    QCOMPARE(rig.heartbeatState(), QStringLiteral("busy"));
}

} // namespace runner_session_test
