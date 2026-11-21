#include "RunnerSessionTest.h"

#include "protocol/FrameLimits.h"
#include "session/OutputBuffer.h"

#include <QTest>

namespace runner_session_test {

using namespace protocol;
using session::OutputBuffer;
using session::outputRingBytes;

namespace {

// Bytes whose value tells their offset apart, so a misplaced page shows.
QByteArray pattern(const qint64 from, const qint64 size) {
    QByteArray bytes(size, Qt::Uninitialized);
    for (qint64 i = 0; i < size; ++i)
        bytes[i] = char((from + i) * 7 % 251);
    return bytes;
}

// Every page sent, checked to be contiguous from `from`, joined.
QByteArray joined(const Host& host, const qint64 from) {
    QByteArray all;
    qint64 next = from;
    for (const JobOutput& page : host.sentOf<JobOutput>()) {
        if (page.offset != next || page.data.size() > limits::maxOutputBytes || page.data.isEmpty())
            return "not contiguous";
        next += page.data.size();
        all.append(page.data);
    }
    return all;
}

} // namespace

void RunnerSessionOutputTest::bufferKeepsTheNewestBytes() {
    OutputBuffer buffer(100);
    buffer.append(pattern(0, 60));
    buffer.append(pattern(60, 60));
    QCOMPARE(buffer.start(), 20);
    QCOMPARE(buffer.end(), 120);
    QCOMPARE(buffer.read(20, 1000), pattern(20, 100));
    QCOMPARE(buffer.read(110, 5), pattern(110, 5));
    QVERIFY(buffer.read(10, 5).isEmpty()); // dropped
    buffer.acknowledge(50);
    QCOMPARE(buffer.start(), 50);
    buffer.acknowledge(500); // never past the end
    QCOMPARE(buffer.start(), 120);
    buffer.append(pattern(120, 300)); // one write above the capacity keeps its newest part
    QCOMPARE(buffer.start(), 320);
    QCOMPARE(buffer.read(320, 1000), pattern(320, 100));
}

void RunnerSessionOutputTest::pagesAreContiguousAndSmall() {
    Host host;
    host.launch(makeSpec());
    host.clearSent();
    const QList<qint64> writes{1, 32 * 1024, 32 * 1024 + 1, 100 * 1024, 7};
    qint64 total = 0;
    for (const qint64 size : writes) {
        host.session->commandOutput(pattern(total, size));
        total += size;
    }
    QCOMPARE(joined(host, 0), pattern(0, total));
    for (const auto& [channel, frame] : host.channel.sent)
        QCOMPARE(channel, Channel::Events);
}

void RunnerSessionOutputTest::ackReleasesTheBuffer() {
    Host host;
    host.launch(makeSpec());
    host.session->commandOutput(pattern(0, 100'000));
    QCOMPARE(host.session->output().start(), 0);
    host.deliver(JobOutputAck{7, QStringLiteral("job-1"), 60'000});
    QCOMPARE(host.session->output().start(), 60'000);
    host.deliver(JobOutputAck{7, QStringLiteral("job-other"), 90'000}); // not this job
    QCOMPARE(host.session->output().start(), 60'000);
}

void RunnerSessionOutputTest::resumeSendsFromTheAskedOffset() {
    Host host;
    host.launch(makeSpec());
    host.session->commandOutput(pattern(0, 50'000));
    // On the same connection.
    host.clearSent();
    host.deliver(JobOutputResume{7, QStringLiteral("job-1"), 10'000});
    QCOMPARE(joined(host, 10'000), pattern(10'000, 40'000));
    // On a new connection: nothing is sent before `job.resume`.
    host.session->channelLost();
    host.session->commandOutput(pattern(50'000, 5'000));
    host.clearSent();
    host.open(QStringLiteral("c2"));
    host.welcome();
    host.session->commandOutput(pattern(55'000, 5'000));
    QCOMPARE(host.sentTypes(), QStringList{"hello"});
    host.clearSent();
    host.deliver(JobResume{7, QStringLiteral("job-1"), 20'000});
    QCOMPARE(host.sentTypes().first(), QStringLiteral("job.started"));
    QCOMPARE(host.sentOf<JobStarted>().first().startedAt, startedAt);
    QCOMPARE(joined(host, 20'000), pattern(20'000, 40'000));
    QCOMPARE(host.session->output().start(), 20'000); // `outputOffset` is in the spool
    QVERIFY(host.sentOf<JobOutputGap>().isEmpty());
}

void RunnerSessionOutputTest::overflowReportsOneGap_data() {
    QTest::addColumn<qint64>("written");
    QTest::addColumn<qint64>("lost");
    QTest::newRow("exactly 16 MiB") << outputRingBytes << qint64(0);
    QTest::newRow("16 MiB and one byte") << outputRingBytes + 1 << qint64(1);
    QTest::newRow("much more") << outputRingBytes + 5'000'000 << qint64(5'000'000);
}

void RunnerSessionOutputTest::overflowReportsOneGap() {
    QFETCH(qint64, written);
    QFETCH(qint64, lost);
    Host host;
    host.launch(makeSpec());
    host.session->channelLost(); // nothing reaches the desktop meanwhile
    const qint64 piece = 1024 * 1024;
    for (qint64 offset = 0; offset < written; offset += piece)
        host.session->commandOutput(pattern(offset, std::min(piece, written - offset)));
    QCOMPARE(host.session->output().start(), lost);
    host.open(QStringLiteral("c2"));
    host.welcome();
    host.clearSent();
    host.deliver(JobResume{7, QStringLiteral("job-1"), 0});
    const QList<JobOutputGap> gaps = host.sentOf<JobOutputGap>();
    if (lost == 0) {
        QVERIFY(gaps.isEmpty());
    } else {
        QCOMPARE(gaps.size(), 1);
        QCOMPARE(gaps.first().fromOffset, 0);
        QCOMPARE(gaps.first().toOffset, lost);
        // The gap comes before the first page, which starts where it ends.
        const QStringList types = host.sentTypes();
        QCOMPARE(types.indexOf("job.output.gap") + 1, types.indexOf("job.output"));
    }
    QCOMPARE(joined(host, lost), pattern(lost, written - lost));
}

void RunnerSessionOutputTest::blockedEventsReportOneGap_data() {
    QTest::addColumn<qint64>("written");
    QTest::addColumn<qint64>("lost");
    QTest::newRow("exactly 16 MiB") << outputRingBytes << qint64(0);
    QTest::newRow("16 MiB and one byte") << outputRingBytes + 1 << qint64(1);
}

// The connection stays, but `events` takes nothing while the command writes.
void RunnerSessionOutputTest::blockedEventsReportOneGap() {
    QFETCH(qint64, written);
    QFETCH(qint64, lost);
    Host host;
    host.launch(makeSpec());
    host.clearSent();
    host.channel.writable = false;
    const qint64 piece = 1024 * 1024;
    for (qint64 offset = 0; offset < written; offset += piece)
        host.session->commandOutput(pattern(offset, std::min(piece, written - offset)));
    QVERIFY(host.channel.sent.isEmpty());
    host.channel.writable = true;
    host.session->eventsWritable();
    const QList<JobOutputGap> gaps = host.sentOf<JobOutputGap>();
    if (lost == 0) {
        QVERIFY(gaps.isEmpty());
    } else {
        QCOMPARE(gaps.size(), 1);
        QCOMPARE(gaps.first().fromOffset, 0);
        QCOMPARE(gaps.first().toOffset, lost);
        QCOMPARE(host.sentTypes().first(), QStringLiteral("job.output.gap"));
    }
    QCOMPARE(joined(host, lost), pattern(lost, written - lost));

    // Bytes that went out and then left the buffer make no gap.
    host.clearSent();
    host.session->commandOutput(pattern(written, outputRingBytes));
    QVERIFY(host.sentOf<JobOutputGap>().isEmpty());
    QCOMPARE(joined(host, written), pattern(written, outputRingBytes));
}

void RunnerSessionOutputTest::resultWaitsForBlockedOutput() {
    Host host;
    host.launch(makeSpec());
    host.clearSent();
    host.channel.writable = false;
    host.session->commandOutput(pattern(0, 100'000));
    host.session->commandFinished({QStringLiteral("exited"), 0, std::nullopt, {}, "verified"});
    QVERIFY(host.channel.sent.isEmpty());
    host.channel.writable = true;
    host.session->eventsWritable();
    QCOMPARE(joined(host, 0), pattern(0, 100'000));
    QCOMPARE(host.sentTypes().last(), QStringLiteral("job.result"));
    QCOMPARE(host.sentOf<JobResult>().size(), 1);
    host.session->eventsWritable(); // nothing more to send
    QCOMPARE(host.sentOf<JobResult>().size(), 1);
}

void RunnerSessionOutputTest::outputBytesCountsEverything() {
    Host host;
    host.launch(makeSpec());
    host.session->channelLost();
    host.session->commandOutput(pattern(0, outputRingBytes));
    host.session->commandOutput(pattern(outputRingBytes, 1234));
    host.session->commandFinished({QStringLiteral("exited"), 0, std::nullopt, {}, "verified"});
    host.open(QStringLiteral("c2"));
    host.welcome();
    host.clearSent();
    host.deliver(JobResume{7, QStringLiteral("job-1"), 0});
    const JobResult result = host.sentOf<JobResult>().first();
    QCOMPARE(result.outputBytes, outputRingBytes + 1234);
    QCOMPARE(host.sentTypes().last(), QStringLiteral("job.result")); // after every page
}

} // namespace runner_session_test
