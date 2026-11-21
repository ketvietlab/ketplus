#include "RunnerDeliveryTest.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QTest>

namespace runner_delivery_test {

QStringList FakeJournal::steps() const {
    static const QStringList names{"acquiring", "leased", "spec_accepted", "started", "closing"};
    QStringList out;
    for (const JournalRecord& record : records)
        out.append(names.at(int(record.step)));
    return out;
}

Host::Host(const RestartPolicy* policy) {
    relay.journalWrites = &journal.writes;
    DeliveryPorts ports{relay,
                        session,
                        journal,
                        trust,
                        policy != nullptr ? *policy : neverRelaunch,
                        clock,
                        [] { return 0.0; },
                        [this] { return QStringLiteral("acq-%1").arg(++nextId); },
                        [this](const QString& message) { reports.append(message); }};
    delivery = std::make_unique<JobDelivery>(
        effectiveLabels({QStringLiteral("gpu")}, QStringLiteral("x86_64")), ports);
}

void Host::advance(const qint64 milliseconds) {
    const qint64 target = clock.now + milliseconds;
    while (true) {
        std::optional<DeliveryTimer> next;
        for (auto it = clock.deadlines.cbegin(); it != clock.deadlines.cend(); ++it)
            if (it.value() <= target && (!next || it.value() < clock.deadlines.value(*next)))
                next = it.key();
        if (!next)
            break;
        clock.now = clock.deadlines.take(*next);
        delivery->timerFired(*next);
    }
    clock.now = target;
}

void Host::answer(const CallResult& result) { delivery->answered(relay.calls.last(), result); }

void Host::refuse(const QString& code, const QJsonObject& body) {
    answer({CallResult::Kind::Refused, code, body});
}

void Host::hold(const QStringList& runsOn) {
    delivery->heartbeatAnswered(true, false);
    answerOk(leaseBody(QStringLiteral("job-1"), QStringLiteral("lease-1"),
                       QStringLiteral("desktop-1"), runsOn));
}

void Host::start() {
    hold();
    delivery->lease().start();
    answerOk({{QStringLiteral("startedAt"), startedAt}});
    session.currentPhase = QStringLiteral("syncing");
}

QStringList Host::callKinds() const {
    static const QStringList names{"acquire", "renew", "start", "finish", "release"};
    QStringList out;
    for (const LeaseCall& call : relay.calls)
        out.append(names.at(int(call.kind)));
    return out;
}

QString Host::heartbeatState() const {
    return delivery->heartbeatFields().value(QStringLiteral("state")).toString();
}

QJsonValue Host::currentJob() const {
    return delivery->heartbeatFields().value(QStringLiteral("currentJob"));
}

QJsonObject leaseBody(const QString& jobId, const QString& leaseId, const QString& machineId,
                      const QStringList& runsOn) {
    return {{QStringLiteral("jobId"), jobId},
            {QStringLiteral("attempt"), 1},
            {QStringLiteral("leaseId"), leaseId},
            {QStringLiteral("leaseExpiresAt"), QStringLiteral("2026-11-18T09:05:00Z")},
            {QStringLiteral("requester"),
             QJsonObject{{QStringLiteral("workspaceId"), QStringLiteral("ws-1")},
                         {QStringLiteral("machineId"), machineId}}},
            {QStringLiteral("runsOn"), QJsonArray::fromStringList(runsOn)}};
}

HeldLease heldLease(const QStringList& runsOn) {
    return *leaseFromReply(leaseBody(QStringLiteral("job-1"), QStringLiteral("lease-1"),
                                     QStringLiteral("desktop-1"), runsOn));
}

} // namespace runner_delivery_test

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    int failures = 0;
    {
        runner_delivery_test::RunnerDeliveryAcquireTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_delivery_test::RunnerDeliveryLeaseTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_delivery_test::RunnerDeliveryRestartTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    return failures == 0 ? 0 : 1;
}
