#include "PeerSessionFakes.h"

#include "protocol/Capabilities.h"

namespace peer_session_test {

using protocol::Channel;
using protocol::Direction;

void FakeLinks::send(const QString& connectionId, const Channel channel,
                     const QByteArray& message) {
    const protocol::DecodeResult decoded =
        protocol::decode(message, channel, Direction::DesktopToRunner);
    if (!decoded.ok()) {
        ++undecodable;
        return;
    }
    sent.append({connectionId, channel, *decoded.frame});
}

RecordAnswer FakeRecords::offer(const QString&, const QString& runnerId, const qint64 attempt) {
    calls.append(QStringLiteral("offer %1 %2").arg(runnerId).arg(attempt));
    if (!offerCode.isEmpty())
        return {offerCode};
    if (!allowAnyOffer && !record.dispatchedTo.isEmpty() && record.dispatchedTo != runnerId)
        return {QStringLiteral("already_dispatched")};
    return {};
}

RecordAnswer FakeRecords::dispatch(const QString&, const QString& runnerId) {
    calls.append(QStringLiteral("dispatch ") + runnerId);
    if (!dispatchCode.isEmpty())
        return {dispatchCode};
    if (!record.dispatchedTo.isEmpty())
        return {QStringLiteral("already_dispatched")};
    record.dispatchedTo = runnerId;
    return {};
}

RecordAnswer FakeRecords::result(const QString&, const QString& runnerId,
                                 const protocol::JobResult& result) {
    calls.append(QStringLiteral("result %1 %2").arg(runnerId, result.state));
    results.append(result);
    if (!resultCodes.isEmpty())
        return {resultCodes.takeFirst()};
    record.terminal = true;
    return {};
}

RecordAnswer FakeRecords::snapshotFailed(const QString&, const QString& runnerId,
                                         const QString& code) {
    calls.append(QStringLiteral("snapshotFailed %1 %2").arg(runnerId, code));
    if (!snapshotCodes.isEmpty())
        return {snapshotCodes.takeFirst()};
    if (record.dispatchedTo != runnerId || !record.accepted || record.started ||
        !record.stopReason.isEmpty() || record.terminal)
        return {QStringLiteral("not_applicable")};
    record.stopReason = QStringLiteral("snapshot_failed");
    return {};
}

std::optional<protocol::JobSpec> FakeSpecs::spec(const QString& id, const qint64 attempt) {
    ++asked;
    if (!available)
        return std::nullopt;
    protocol::JobSpec spec = makeSpec(attempt);
    spec.jobId = id;
    return spec;
}

bool MemorySpool::append(const QByteArray& data) {
    if (failing)
        return false;
    bytes.append(data);
    size_ += data.size();
    ++writes;
    return true;
}

bool MemorySpool::markGap(const qint64 toOffset) {
    if (failing)
        return false;
    gaps.append({size_, toOffset});
    ++writes;
    size_ = toOffset;
    return true;
}

protocol::JobSpec makeSpec(const qint64 attempt) {
    protocol::JobSpec spec;
    spec.jobId = jobId;
    spec.attempt = attempt;
    spec.invocation.argv = QStringList{QStringLiteral("make"), QStringLiteral("test")};
    spec.cwd = QStringLiteral("src");
    spec.timeoutMs = 60'000;
    spec.repositoryKey = QString(32, QLatin1Char('r'));
    spec.workspaceKey = QString(32, QLatin1Char('w'));
    spec.snapshotId = QString(40, QLatin1Char('a'));
    spec.sourceHead = QString(40, QLatin1Char('b'));
    spec.objectFormat = QStringLiteral("sha1");
    return spec;
}

protocol::Hello makeHello(const QString& connectionId, const QString& runner,
                          const qint64 attempt) {
    protocol::Hello hello;
    hello.connectionId = connectionId;
    hello.capabilities = {QString(protocol::capability::runnerProtocol)};
    hello.runnerId = runner;
    hello.runnerVersion = QStringLiteral("0.1.0");
    hello.os = QStringLiteral("linux");
    hello.arch = QStringLiteral("x86_64");
    hello.labels = {QStringLiteral("os:linux"), QStringLiteral("gpu")};
    hello.jobId = jobId;
    hello.attempt = attempt;
    hello.snapshots = {{QString(32, QLatin1Char('w')), QString(40, QLatin1Char('c'))}};
    return hello;
}

QByteArray encodeFromRunner(const protocol::Frame& frame) {
    return protocol::encode(frame, Direction::RunnerToDesktop).message;
}

Host::Host() {
    records.record.runsOn = {QStringLiteral("os:linux")};
    session = std::make_unique<JobPeerSession>(
        jobId, daemonVersion, SessionPorts{links, clock, records, specs, snapshots, spool});
}

void Host::advance(const qint64 milliseconds) {
    const QDateTime end = clock.time.addMSecs(milliseconds);
    for (;;) {
        std::optional<PeerTimer> next;
        for (auto it = clock.deadlines.cbegin(); it != clock.deadlines.cend(); ++it)
            if (it.value() <= end && (!next || it.value() < clock.deadlines.value(*next)))
                next = it.key();
        if (!next)
            break;
        clock.time = clock.deadlines.take(*next);
        session->timerFired(*next);
    }
    clock.time = end;
}

void Host::open(const QString& connectionId, const QString& runner, const qint64 attempt) {
    const RecordAnswer answer = session->offered(connectionId, runner, attempt);
    if (answer.ok())
        session->channelsOpened(connectionId);
}

void Host::connect(const QString& connectionId, const QString& runner, const qint64 attempt) {
    open(connectionId, runner, attempt);
    deliver(makeHello(connectionId, runner, attempt), connectionId);
}

void Host::accept() {
    connect();
    deliver(protocol::JobAccepted{jobId, 1});
}

void Host::start() {
    accept();
    deliver(protocol::JobStarted{jobId, QStringLiteral("2026-11-18T09:00:00Z")});
}

void Host::deliver(const protocol::Frame& frame, const QString& connectionId) {
    const protocol::Route* route = protocol::routeFor(protocol::frameType(frame));
    deliverRaw(route->channel, encodeFromRunner(frame), connectionId);
}

void Host::deliverRaw(const Channel channel, const QByteArray& message,
                      const QString& connectionId) {
    session->frameReceived(connectionId, channel, message);
}

QStringList Host::sentTypes() const {
    QStringList types;
    for (const SentFrame& sent : links.sent)
        types.append(QString(protocol::frameType(sent.frame)));
    return types;
}

} // namespace peer_session_test
