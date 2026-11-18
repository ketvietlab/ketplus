#include "RunnerPeerTest.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingSignatures.h"

#include <QCoreApplication>
#include <QMutex>
#include <QtTest>

namespace runner_peer_test {

namespace cc = ketplus::control_crypto;

QDateTime vectorTime() {
    return QDateTime::fromString(QString::fromLatin1(vectors::issuedAt), Qt::ISODateWithMs);
}

QJsonArray iceServers() {
    return {QJsonObject{{QStringLiteral("urls"), QStringLiteral("turn:127.0.0.1:3478")},
                        {QStringLiteral("username"), QStringLiteral("TURN-SECRET-user")},
                        {QStringLiteral("credential"), QStringLiteral("TURN-SECRET-pass")}}};
}

pairing::RunnerPin activePin() {
    pairing::RunnerPin pin;
    pin.machineId = QString::fromLatin1(vectors::machineId);
    pin.workspaceId = QString::fromLatin1(vectors::workspaceId);
    pin.machineKeyVersion = vectors::machineKeyVersion;
    pin.signingPublicKey = QByteArray::fromHex(vectors::desktopPublicKey);
    pin.state = pairing::RunnerPin::State::Active;
    pin.pairedAt = vectorTime();
    return pin;
}

QString answerFingerprint() {
    QStringList pairs;
    for (int i = 0; i < 32; ++i)
        pairs.append(QStringLiteral("%1").arg(i * 7 % 256, 2, 16, QLatin1Char('0')).toUpper());
    return QStringLiteral("sha-256 ") + pairs.join(QLatin1Char(':'));
}

QString answerSdp(const bool withFingerprint) {
    QString sdp = QStringLiteral("v=0\r\no=- 3 4 IN IP4 127.0.0.1\r\n");
    if (withFingerprint)
        sdp += QStringLiteral("a=fingerprint:") + answerFingerprint() + QStringLiteral("\r\n");
    return sdp;
}

QJsonObject signedAnswer(const QString& connectionId, const QString& offerSdp, const QString& sdp,
                         const QDateTime& issuedAt, const int keyVersion, const QString& jobId) {
    pairing::AnswerPayload payload;
    payload.connectionId = connectionId;
    payload.workspaceId = QString::fromLatin1(vectors::workspaceId);
    payload.machineId = QString::fromLatin1(vectors::machineId);
    payload.machineKeyVersion = keyVersion;
    payload.runnerId = QString::fromLatin1(vectors::runnerId);
    payload.jobId = jobId;
    payload.offerSdp = offerSdp;
    payload.issuedAt = pairing::issuedAtString(issuedAt);
    payload.sdp = sdp;
    const QByteArray signature =
        pairing::signPayload(QByteArray::fromHex(vectors::desktopSeed), payload.bytes());
    return {{QStringLiteral("type"), QStringLiteral("signal.answer")},
            {QStringLiteral("connectionId"), connectionId},
            {QStringLiteral("sdp"), sdp},
            {QStringLiteral("machineKeyVersion"), keyVersion},
            {QStringLiteral("issuedAt"), payload.issuedAt},
            {QStringLiteral("signature"), QString::fromLatin1(cc::base64Url(signature))}};
}

QStringList& logLines() {
    static QStringList lines;
    return lines;
}

void FakeClock::advance(const qint64 ms) {
    const qint64 target = elapsed + ms;
    for (;;) {
        auto next = due.cend();
        for (auto it = due.cbegin(); it != due.cend(); ++it)
            if (it.value() <= target && (next == due.cend() || it.value() < next.value()))
                next = it;
        if (next == due.cend())
            break;
        const PeerTimer timer = next.key();
        elapsed = std::max(elapsed, next.value());
        due.remove(timer);
        if (fire)
            fire(timer);
    }
    elapsed = target;
}

void FakeRelaySocket::open(const QString& accessToken) {
    if (live)
        ++openedWhileLive;
    live = true;
    tokens.append(accessToken);
}

bool FakeRelaySocket::send(const QJsonObject& message) {
    if (!live)
        return false;
    sent.append(message);
    return true;
}

void FakeTokens::take(const QString& call, Answer answer) {
    calls.append(call);
    pending.append(std::move(answer));
    if (autoAnswer)
        this->answer(Outcome::Ok);
}

void FakeTokens::answer(const Outcome outcome) {
    if (pending.isEmpty())
        return;
    // As the real source: current() hands out the held token, renew() a new one.
    const bool renewal = calls.at(calls.size() - pending.size()) == QLatin1StringView("renew");
    const Answer next = pending.takeFirst();
    Token token{outcome, {}};
    if (outcome == Outcome::Ok) {
        if (renewal || issued == 0)
            ++issued;
        token.value = QStringLiteral("%1-%2").arg(QLatin1StringView(tokenMarker)).arg(issued);
    }
    next(token);
}

void RecordingRelayListener::jobAvailable(const QString& jobId) {
    wakeups.append(QStringLiteral("job.available ") + jobId);
}

void RecordingRelayListener::jobCancel(const QString& jobId, const QString& leaseId) {
    wakeups.append(QStringLiteral("job.cancel %1 %2").arg(jobId, leaseId));
}

void RecordingRelayListener::runnerPaired(const QString& ticketId, const QString& pairState,
                                          const QString& approvalSignature) {
    wakeups.append(
        QStringLiteral("runner.paired %1 %2 %3").arg(ticketId, pairState, approvalSignature));
}

void RecordingRelayListener::runnerUnpaired(const QString& machineId) {
    wakeups.append(QStringLiteral("runner.unpaired ") + machineId);
}

namespace {

class FakeTransport final : public PeerTransport {
  public:
    explicit FakeTransport(std::shared_ptr<TransportRecord> record) : m_record(std::move(record)) {}
    ~FakeTransport() override { m_record->destroyed = true; }

    bool offer(const QJsonArray& servers) override {
        m_record->iceServers = servers;
        m_record->offered = true;
        return m_record->offerOk;
    }
    bool acceptAnswer(const QString& sdp, const QString& fingerprint) override {
        m_record->answers.append(sdp);
        m_record->fingerprints.append(fingerprint);
        return m_record->acceptOk;
    }
    bool candidate(const QString& candidate, const QString& mid) override {
        m_record->candidates.append(mid + QLatin1Char('|') + candidate);
        return true;
    }
    bool send(Channel channel, const QByteArray& bytes, bool binary) override {
        m_record->sent.append({channel, bytes, binary});
        return true;
    }
    [[nodiscard]] quint64 buffered(Channel channel) const override {
        return channel == Channel::Events ? m_record->bufferedEvents : 0;
    }
    void close() override { m_record->closed = true; }

  private:
    std::shared_ptr<TransportRecord> m_record;
};

} // namespace

std::unique_ptr<PeerTransport> FakeTransports::create(PeerTransportEvents&) {
    auto record = std::make_shared<TransportRecord>();
    record->offerOk = offerOk;
    records.append(record);
    return std::make_unique<FakeTransport>(record);
}

QString FakeSigner::runnerId() const { return QString::fromLatin1(vectors::runnerId); }

QByteArray FakeSigner::sign(const QByteArray& bytes) const {
    return pairing::signPayload(QByteArray::fromHex(vectors::runnerSeed), bytes);
}

std::optional<pairing::RunnerPin> FakePins::activePin(const QString& machineId) const {
    if (!pin || pin->machineId != machineId || pin->state != pairing::RunnerPin::State::Active)
        return std::nullopt;
    return pin;
}

bool FakeSender::sendSignal(const QJsonObject& message) {
    if (!ready)
        return false;
    sent.append(message);
    return true;
}

QList<QJsonObject> FakeSender::ofType(const QString& type) const {
    QList<QJsonObject> found;
    for (const auto& message : sent)
        if (message.value(QStringLiteral("type")).toString() == type)
            found.append(message);
    return found;
}

void RecordingChannelListener::channelOpened(const QString& connectionId) {
    events.append(QStringLiteral("opened ") + connectionId);
}

void RecordingChannelListener::channelLost(const QString& reason) {
    events.append(QStringLiteral("lost ") + reason);
}

void RecordingChannelListener::frameReceived(protocol::Channel channel, const QByteArray& message) {
    frames.append({channel, message});
}

void RecordingChannelListener::transferReceived(quint64 transferId, const QByteArray& bytes) {
    transfers.append({transferId, bytes});
}

void RecordingChannelListener::attemptFailed(const QString& reason) {
    events.append(QStringLiteral("failed ") + reason);
}

void RecordingChannelListener::gaveUp(const QString& reason) {
    events.append(QStringLiteral("gaveUp ") + reason);
}

PeerHarness::PeerHarness() {
    channels.attach(connector);
    clock.fire = [this](PeerTimer timer) { connector.timerFired(timer); };
}

void PeerHarness::request(const int attempt) {
    PeerRequest request{QString::fromLatin1(vectors::jobId), QStringLiteral("lease_1"), attempt,
                        QString::fromLatin1(vectors::machineId)};
    channels.setRequest(request);
    connector.request(request);
}

void PeerHarness::started(const QString& id, const qint64 expiresInMs) {
    connector.signal({{QStringLiteral("type"), QStringLiteral("signal.started")},
                      {QStringLiteral("connectionId"), id},
                      {QStringLiteral("iceServers"), iceServers()},
                      {QStringLiteral("expiresAt"),
                       clock.now().addMSecs(expiresInMs).toString(Qt::ISODateWithMs)}});
}

void PeerHarness::describe(const QString& sdp) {
    offerSdp = sdp;
    connector.localDescription(sdp);
}

void PeerHarness::answer(const QString& id) {
    connector.signal(signedAnswer(id, offerSdp, answerSdp(), clock.now()));
}

void PeerHarness::open() {
    request();
    started();
    describe();
    answer();
    connector.opened();
}

} // namespace runner_peer_test

namespace {

QMutex logMutex;
QtMessageHandler previousHandler = nullptr;

void recordLog(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    {
        QMutexLocker lock(&logMutex);
        runner_peer_test::logLines().append(message);
    }
    if (previousHandler)
        previousHandler(type, context, message);
}

} // namespace

void runner_peer_test::captureLogs() { previousHandler = qInstallMessageHandler(recordLog); }

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QLoggingCategory::setFilterRules(QStringLiteral("ketplus.runner.peer.debug=true"));
    int failures = 0;
    {
        runner_peer_test::RunnerRelayLinkTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_peer_test::RunnerPeerConnectorTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_peer_test::RunnerPeerChannelsTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_peer_test::RunnerPeerLoopbackTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    return failures == 0 ? 0 : 1;
}
