#include "RunnerLinkTest.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingSignatures.h"
#include "runner/peer/RemotePeerSessions.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QMutex>
#include <QtTest>

namespace runner_link_test {
namespace cc = ketplus::control_crypto;

// The links send through the peer sessions' port, and their job side is named and typed as the
// sessions' entry points, so the two join without a translating layer.
namespace sessions_match {
using Sessions = ketplus::daemon::runner::peer::RemotePeerSessions;
static_assert(std::is_base_of_v<ketplus::daemon::runner::peer::PeerLinks, RunnerLinks>);
static_assert(std::is_same_v<decltype(&Sessions::offered),
                             RecordAnswer (Sessions::*)(const QString&, const QString&,
                                                        const QString&, qint64)>);
static_assert(std::is_same_v<decltype(&Sessions::offerEnded), void (Sessions::*)(const QString&)>);
static_assert(
    std::is_same_v<decltype(&Sessions::channelsOpened), void (Sessions::*)(const QString&)>);
static_assert(std::is_same_v<decltype(&Sessions::frameReceived),
                             void (Sessions::*)(const QString&, FrameChannel, const QByteArray&)>);
static_assert(
    std::is_same_v<decltype(&Sessions::bulkWritable), void (Sessions::*)(const QString&)>);
static_assert(
    std::is_same_v<decltype(&Sessions::connectionLost), void (Sessions::*)(const QString&)>);
} // namespace sessions_match

QDateTime vectorTime() {
    return QDateTime::fromString(QString::fromLatin1(vectors::issuedAt), Qt::ISODateWithMs);
}

QJsonArray iceServers() {
    return {QJsonObject{{QStringLiteral("urls"), QStringLiteral("turn:127.0.0.1:3478")},
                        {QStringLiteral("username"), QStringLiteral("TURN-SECRET-user")},
                        {QStringLiteral("credential"), QStringLiteral("TURN-SECRET-pass")}}};
}

QString connectionId(const int n) {
    return QStringLiteral("pc_") + QString::number(n).rightJustified(24, QLatin1Char('0'));
}

QString runnerId(const int n) {
    return QStringLiteral("run_") + QString::number(n).rightJustified(12, QLatin1Char('0'));
}

pairing::DesktopRunnerPin runnerPin(const QString& runnerId) {
    pairing::DesktopRunnerPin pin;
    pin.runnerId = runnerId;
    pin.runnerKeyVersion = vectors::runnerKeyVersion;
    pin.publicKey = QByteArray::fromHex(vectors::runnerPublicKey);
    pin.name = QStringLiteral("builder");
    pin.pairedAt = vectorTime();
    return pin;
}

QJsonObject offer(const OfferFields& fields) {
    pairing::OfferPayload payload{fields.connectionId,
                                  QString::fromLatin1(vectors::workspaceId),
                                  QString::fromLatin1(vectors::machineId),
                                  fields.runnerId,
                                  fields.runnerKeyVersion,
                                  fields.jobId,
                                  fields.attempt,
                                  fields.issuedAt,
                                  fields.sdp};
    const QByteArray signature =
        pairing::signPayload(QByteArray::fromHex(vectors::runnerSeed), payload.bytes());
    return {{QStringLiteral("type"), QStringLiteral("signal.offer")},
            {QStringLiteral("connectionId"), fields.connectionId},
            {QStringLiteral("sdp"), fields.sdp},
            {QStringLiteral("jobId"), fields.jobId},
            {QStringLiteral("attempt"), fields.attempt},
            {QStringLiteral("runnerKeyVersion"), fields.runnerKeyVersion},
            {QStringLiteral("issuedAt"), fields.issuedAt},
            {QStringLiteral("signature"), QString::fromLatin1(cc::base64Url(signature))},
            {QStringLiteral("runnerId"), fields.runnerId},
            {QStringLiteral("iceServers"), iceServers()}};
}

QStringList& logLines() {
    static QStringList lines;
    return lines;
}

void FakeClock::advance(const qint64 ms) {
    const qint64 until = elapsed + ms;
    for (;;) {
        QString next;
        qint64 at = until + 1;
        for (auto it = due.cbegin(); it != due.cend(); ++it)
            if (it.value() <= until && it.value() < at) {
                at = it.value();
                next = it.key();
            }
        if (next.isEmpty())
            break;
        elapsed = at;
        due.remove(next);
        if (fire)
            fire(next);
    }
    elapsed = until;
}

bool FakeTransport::answer(const QString& offerSdp, const QJsonArray& iceServers) {
    m_record->offerSdp = offerSdp;
    m_record->iceServers = iceServers;
    if (m_record->duringAnswer)
        m_record->duringAnswer();
    return m_record->answerOk;
}

bool FakeTransport::candidate(const QString& candidate, const QString& mid) {
    m_record->candidates.append({candidate, mid});
    return m_record->candidateOk;
}

bool FakeTransport::send(const TransportChannel channel, const QByteArray& bytes,
                         const bool binary) {
    if (!m_record->sendOk)
        return false;
    m_record->sent.append({channel, bytes, binary});
    return true;
}

std::unique_ptr<LinkTransport> FakeTransports::create(LinkTransportEvents& events) {
    auto record = std::make_shared<TransportRecord>();
    record->events = &events;
    record->answerOk = answerOk;
    record->duringAnswer = duringAnswer;
    records.append(record);
    return std::make_unique<FakeTransport>(record);
}

QByteArray FakeIdentity::sign(const QByteArray& bytes) const {
    if (fail)
        return {};
    return pairing::signPayload(QByteArray::fromHex(vectors::desktopSeed), bytes);
}

std::optional<pairing::DesktopRunnerPin> FakePins::pin(const QString& runnerId) const {
    if (unpinned.contains(runnerId))
        return std::nullopt;
    if (overrides.contains(runnerId))
        return overrides.value(runnerId);
    return runnerPin(runnerId);
}

QList<QJsonObject> FakeOut::ofType(const QString& type) const {
    QList<QJsonObject> matching;
    for (const QJsonObject& message : sent)
        if (message.value(QStringLiteral("type")).toString() == type)
            matching.append(message);
    return matching;
}

QStringList FakeOut::ends() const {
    QStringList reasons;
    for (const QJsonObject& message : ofType(QStringLiteral("signal.end")))
        reasons.append(message.value(QStringLiteral("reason")).toString());
    return reasons;
}

RecordAnswer RecordingEvents::offered(const QString& connectionId, const QString& runnerId,
                                      const QString& jobId, const qint64 attempt) {
    events.append(
        QStringLiteral("offered %1 %2 %3 %4").arg(connectionId, runnerId, jobId).arg(attempt));
    return {code};
}

void RecordingEvents::connectionLost(const QString& connectionId) {
    static const QString prefix = QStringLiteral("runner peer connection lost: \"");
    QString reason = QStringLiteral("?");
    const QString line = logLines().isEmpty() ? QString() : logLines().last();
    if (const qsizetype at = line.indexOf(prefix); at >= 0)
        reason = line.mid(at + prefix.size()).chopped(1);
    events.append(QStringLiteral("lost %1 %2").arg(connectionId, reason));
}

void RecordingEvents::frameReceived(const QString& connectionId, const FrameChannel channel,
                                    const QByteArray& message) {
    frames.append({connectionId, channel, message});
}

LinkHarness::LinkHarness() {
    clock.fire = [this](const QString& id) { links.timerFired(id); };
}

TransportRecord& LinkHarness::open(const OfferFields& fields) {
    deliver(offer(fields));
    TransportRecord& record = transports.last();
    record.events->localDescription(QString::fromLatin1(vectors::answerSdp));
    record.events->opened();
    return record;
}

} // namespace runner_link_test

namespace {

QMutex logMutex;
QtMessageHandler previousHandler = nullptr;

void recordLog(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    {
        QMutexLocker lock(&logMutex);
        runner_link_test::logLines().append(message);
    }
    if (previousHandler)
        previousHandler(type, context, message);
}

} // namespace

void runner_link_test::captureLogs() { previousHandler = qInstallMessageHandler(recordLog); }

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QLoggingCategory::setFilterRules(QStringLiteral("ketplus.daemon.runner.link.debug=true"));
    int failures = 0;
    {
        runner_link_test::RunnerLinkOfferTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_link_test::RunnerLinkLifecycleTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_link_test::RunnerLinkChannelsTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_link_test::RunnerLinkLoopbackTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    return failures == 0 ? 0 : 1;
}
