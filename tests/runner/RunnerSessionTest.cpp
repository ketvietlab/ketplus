#include "RunnerSessionTest.h"

#include "protocol/FrameRoutes.h"

#include <QCoreApplication>
#include <QTest>
#include <QTimeZone>

namespace runner_session_test {

void FakeChannel::send(const protocol::Channel channel, const QByteArray& message) {
    const protocol::DecodeResult decoded =
        protocol::decode(message, channel, protocol::Direction::RunnerToDesktop);
    if (!decoded.ok()) {
        ++undecodable;
        return;
    }
    sent.append({channel, *decoded.frame});
}

qint64 FakeClock::remaining(const SessionTimer timer) const {
    return deadlines.contains(timer) ? time.msecsTo(deadlines.value(timer)) : -1;
}

Host::Host(session::AcceptedJobs* acceptedJobs, session::JobLease* jobLease) {
    clock.time = QDateTime(QDate(2026, 11, 18), QTime(9, 0), QTimeZone::UTC);
    session::RunnerInfo runner;
    runner.runnerId = QStringLiteral("runner-1");
    runner.runnerVersion = QStringLiteral("1.2.3");
    runner.os = QStringLiteral("linux");
    runner.arch = QStringLiteral("x86_64");
    runner.labels = {QStringLiteral("gpu")};
    runner.baseEnvironment = {{QStringLiteral("PATH"), QStringLiteral("/usr/bin")},
                              {QStringLiteral("HOME"), QStringLiteral("/home/runner")},
                              {QStringLiteral("LANG"), QStringLiteral("C.UTF-8")}};
    session = std::make_unique<session::JobSession>(
        runner,
        session::SessionPorts{channel, jobLease != nullptr ? *jobLease : lease, snapshots, command,
                              clock, acceptedJobs != nullptr ? *acceptedJobs : accepted});
}

void Host::advance(const qint64 milliseconds) {
    const QDateTime target = clock.time.addMSecs(milliseconds);
    while (true) {
        std::optional<SessionTimer> next;
        for (auto it = clock.deadlines.cbegin(); it != clock.deadlines.cend(); ++it)
            if (it.value() <= target && (!next || it.value() < clock.deadlines.value(*next)))
                next = it.key();
        if (!next)
            break;
        clock.time = clock.deadlines.take(*next);
        session->timerFired(*next);
    }
    clock.time = target;
}

void Host::deliver(const protocol::Frame& frame) {
    const protocol::EncodeResult encoded =
        protocol::encode(frame, protocol::Direction::DesktopToRunner);
    QVERIFY2(encoded.ok(), "a test frame breaks a rule");
    const protocol::Route* route = protocol::routeFor(protocol::frameType(frame));
    session->frameReceived(route->channel, encoded.message);
}

void Host::deliverRaw(const protocol::Channel channel, const QByteArray& message) {
    session->frameReceived(channel, message);
}

void Host::open(const QString& id) {
    connectionId = id;
    session->channelOpened(id);
}

void Host::welcome(const QStringList& capabilities) {
    deliver(protocol::Welcome{connectionId, 7, capabilities, QStringLiteral("9.9.9")});
}

void Host::connect() {
    QVERIFY(session->assign(assignment()));
    open();
    welcome();
}

void Host::accept(const protocol::JobSpec& spec) {
    connect();
    deliver(spec);
    session->leaseStarted(startedAt);
}

void Host::launch(const protocol::JobSpec& spec) {
    accept(spec);
    session->snapshotReady(QStringLiteral("/work/ws"), {});
}

QStringList Host::sentTypes() const {
    QStringList types;
    for (const auto& [ch, frame] : channel.sent)
        types.append(QString(protocol::frameType(frame)));
    return types;
}

protocol::JobSpec makeSpec(const QString& jobId, const qint64 attempt) {
    protocol::JobSpec spec;
    spec.epoch = 7;
    spec.jobId = jobId;
    spec.attempt = attempt;
    spec.invocation.argv = QStringList{QStringLiteral("make"), QStringLiteral("test")};
    spec.cwd = QStringLiteral("src");
    spec.env = {{QStringLiteral("CC"), QStringLiteral("clang")},
                {QStringLiteral("LANG"), std::nullopt}};
    spec.timeoutMs = 60'000;
    spec.repositoryKey = QString(32, QLatin1Char('r'));
    spec.workspaceKey = QString(32, QLatin1Char('w'));
    spec.snapshotId = QString(40, QLatin1Char('a'));
    spec.sourceHead = QString(40, QLatin1Char('b'));
    spec.objectFormat = QStringLiteral("sha1");
    spec.clean = false;
    return spec;
}

session::JobAssignment assignment(const QString& jobId, const qint64 attempt) {
    return {jobId, attempt, QStringLiteral("desktop-1")};
}

} // namespace runner_session_test

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    int failures = 0;
    {
        runner_session_test::RunnerSessionHandshakeTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_session_test::RunnerSessionSpecTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_session_test::RunnerSessionOutputTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_session_test::RunnerSessionDeliveryTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_session_test::RunnerSessionStopTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_session_test::RunnerSessionLostOutputTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    return failures == 0 ? 0 : 1;
}
