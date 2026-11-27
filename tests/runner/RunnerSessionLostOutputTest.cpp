#include "RunnerSessionTest.h"

#include "session/OutputBuffer.h"

#include <QTest>

namespace runner_session_test {

using namespace protocol;
using session::OutputBuffer;

namespace {

QByteArray bytes(const char value, const qint64 size) { return QByteArray(size, value); }

// The frames sent, as "output <offset> <size>" and "gap <from> <to>".
QStringList outputFrames(const Host& host) {
    QStringList frames;
    for (const auto& [channel, frame] : host.channel.sent) {
        if (const auto* page = std::get_if<JobOutput>(&frame))
            frames.append(QStringLiteral("output %1 %2").arg(page->offset).arg(page->data.size()));
        else if (const auto* gap = std::get_if<JobOutputGap>(&frame))
            frames.append(QStringLiteral("gap %1 %2").arg(gap->fromOffset).arg(gap->toOffset));
    }
    return frames;
}

} // namespace

void RunnerSessionLostOutputTest::bufferNeverHoldsALostRange() {
    OutputBuffer buffer(100);
    buffer.append(bytes('a', 10));
    buffer.skip(5);
    buffer.append(bytes('b', 10));
    QCOMPARE(buffer.start(), 0);
    QCOMPARE(buffer.end(), 25);
    QCOMPARE(buffer.heldFrom(3), 3);
    QCOMPARE(buffer.heldFrom(10), 15);
    QCOMPARE(buffer.heldFrom(25), 25);
    QCOMPARE(buffer.read(0, 100), bytes('a', 10)); // stops at the lost range
    QVERIFY(buffer.read(12, 100).isEmpty());
    QCOMPARE(buffer.read(15, 100), bytes('b', 10));
    buffer.acknowledge(12);
    QCOMPARE(buffer.start(), 15);
    buffer.skip(5); // nothing held after it
    QCOMPARE(buffer.heldFrom(25), 30);
    buffer.acknowledge(30);
    QCOMPARE(buffer.start(), 30);

    // The capacity counts held bytes only.
    OutputBuffer small(20);
    small.append(bytes('a', 10));
    small.skip(1000);
    small.append(bytes('b', 10));
    QCOMPARE(small.start(), 0);
    small.append(bytes('c', 5));
    QCOMPARE(small.start(), 5);
    QCOMPARE(small.read(5, 100), bytes('a', 5));
}

void RunnerSessionLostOutputTest::lostBytesMakeOneGapInOrder() {
    Host host;
    host.launch(makeSpec());
    host.clearSent();
    host.session->commandOutput(bytes('a', 100));
    host.session->commandOutputLost(50);
    host.session->commandOutput(bytes('b', 30));
    host.session->commandOutputLost(0); // nothing
    QCOMPARE(outputFrames(host), QStringList({"output 0 100", "gap 100 150", "output 150 30"}));
    host.session->commandFinished({QStringLiteral("exited"), 0, std::nullopt, {}, "verified"});
    QCOMPARE(host.sentOf<JobResult>().first().outputBytes, 180);
}

// While `events` takes nothing, the bytes held before the lost range still go first.
void RunnerSessionLostOutputTest::lostBytesWhileBlockedJoinTheGap() {
    Host host;
    host.launch(makeSpec());
    host.clearSent();
    host.channel.writable = false;
    host.session->commandOutput(bytes('a', 10));
    host.session->commandOutputLost(20);
    host.session->commandOutputLost(5);
    host.session->commandOutput(bytes('b', 7));
    QVERIFY(host.channel.sent.isEmpty());
    host.channel.writable = true;
    host.session->eventsWritable();
    QCOMPARE(outputFrames(host), QStringList({"output 0 10", "gap 10 35", "output 35 7"}));

    // Asked again from the start: the same ranges, the lost one again a gap.
    host.clearSent();
    host.deliver(JobOutputResume{7, QStringLiteral("job-1"), 0});
    QCOMPARE(outputFrames(host), QStringList({"output 0 10", "gap 10 35", "output 35 7"}));
}

void RunnerSessionLostOutputTest::lostBytesOutsideACommandAreIgnored() {
    Host host;
    host.session->commandOutputLost(10);
    host.accept(makeSpec());
    host.session->commandOutputLost(10); // still syncing
    host.session->snapshotReady(QStringLiteral("/work/ws"), {});
    host.clearSent();
    host.session->commandOutput(bytes('a', 4));
    QCOMPARE(outputFrames(host), QStringList({"output 0 4"}));
}

void RunnerSessionLostOutputTest::launchCarriesTheJobAndItsTimeLimit() {
    Host host;
    JobSpec spec = makeSpec();
    spec.timeoutMs = 123'456;
    host.launch(spec);
    QCOMPARE(host.command.launches.size(), 1);
    const session::LaunchRequest& request = host.command.launches.first();
    QCOMPARE(request.jobId, spec.jobId);
    QCOMPARE(request.attempt, spec.attempt);
    QCOMPARE(request.timeoutMs, 123'456);
}

} // namespace runner_session_test
