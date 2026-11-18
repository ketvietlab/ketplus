#include "RunnerRunTest.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingIdentity.h"
#include "pairing/PairingSignatures.h"
#include "protocol/Capabilities.h"
#include "protocol/FrameRoutes.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
#include <QTimer>

namespace runner_run_test {

namespace cc = ketplus::control_crypto;
using delivery::CallResult;
using delivery::LeaseCall;

namespace {

const QString offerSdp = QStringLiteral("v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\n");

QString answerSdp() {
    QStringList pairs;
    for (int i = 0; i < 32; ++i)
        pairs.append(QStringLiteral("%1").arg(i * 7 % 256, 2, 16, QLatin1Char('0')).toUpper());
    return QStringLiteral("v=0\r\no=- 3 4 IN IP4 127.0.0.1\r\na=fingerprint:sha-256 ") +
           pairs.join(QLatin1Char(':')) + QStringLiteral("\r\n");
}

QJsonObject signedAnswer(const QString& connectionId, const QString& jobId,
                         const QDateTime& issuedAt) {
    return signedAnswerFor(connectionId, jobId, offerSdp, answerSdp(), issuedAt);
}

} // namespace

QJsonObject signedAnswerFor(const QString& connectionId, const QString& jobId, const QString& offer,
                            const QString& sdp, const QDateTime& issuedAt) {
    pairing::AnswerPayload payload;
    payload.connectionId = connectionId;
    payload.workspaceId = QString::fromLatin1(vectors::workspaceId);
    payload.machineId = QString::fromLatin1(vectors::machineId);
    payload.machineKeyVersion = 1;
    payload.runnerId = QString::fromLatin1(vectors::runnerId);
    payload.jobId = jobId;
    payload.offerSdp = offer;
    payload.issuedAt = pairing::issuedAtString(issuedAt);
    payload.sdp = sdp;
    const QByteArray signature =
        pairing::signPayload(QByteArray::fromHex(vectors::desktopSeed), payload.bytes());
    return {{QStringLiteral("type"), QStringLiteral("signal.answer")},
            {QStringLiteral("connectionId"), connectionId},
            {QStringLiteral("sdp"), payload.sdp},
            {QStringLiteral("machineKeyVersion"), 1},
            {QStringLiteral("issuedAt"), payload.issuedAt},
            {QStringLiteral("signature"), QString::fromLatin1(cc::base64Url(signature))}};
}

namespace {

QString kindName(const LeaseCall::Kind kind) {
    switch (kind) {
    case LeaseCall::Kind::Acquire:
        return QStringLiteral("acquire");
    case LeaseCall::Kind::Renew:
        return QStringLiteral("renew");
    case LeaseCall::Kind::Start:
        return QStringLiteral("start");
    case LeaseCall::Kind::Finish:
        return QStringLiteral("finish");
    case LeaseCall::Kind::Release:
        return QStringLiteral("release");
    }
    return {};
}

class FakeTransport final : public peer::PeerTransport {
  public:
    explicit FakeTransport(std::shared_ptr<TransportState> state) : m_state(std::move(state)) {}
    ~FakeTransport() override { m_state->alive = false; }
    bool offer(const QJsonArray&) override { return m_state->offered = true; }
    bool acceptAnswer(const QString& sdp, const QString&) override {
        m_state->answers.append(sdp);
        return true;
    }
    bool candidate(const QString&, const QString&) override { return true; }
    bool send(peer::Channel channel, const QByteArray& bytes, bool binary) override {
        m_state->sent.append({channel, bytes, binary});
        return true;
    }
    [[nodiscard]] quint64 buffered(peer::Channel) const override { return 0; }
    void close() override { m_state->closed = true; }

  private:
    std::shared_ptr<TransportState> m_state;
};

} // namespace

// FakeHub.
void FakeHub::arm(const TimerKey key, const qint64 delayMs) {
    m_due.insert(code(key), {key, elapsed + delayMs});
    arms.append({key, delayMs});
    if (onArm)
        onArm(key, delayMs);
}

void FakeHub::disarm(const TimerKey key) { m_due.remove(code(key)); }

bool FakeHub::armed(const TimerKey key) const { return m_due.contains(code(key)); }

qint64 FakeHub::remaining(const TimerKey key) const {
    return m_due.value(code(key)).second - elapsed;
}

std::optional<std::pair<TimerKey, qint64>> FakeHub::next() const {
    std::optional<std::pair<TimerKey, qint64>> found;
    for (const auto& [key, due] : m_due)
        if (!found || due < found->second)
            found = std::pair{key, due};
    return found;
}

bool FakeHub::fireNext() {
    const auto due = next();
    if (!due)
        return false;
    m_due.remove(code(due->first));
    elapsed = std::max(elapsed, due->second);
    if (m_fired)
        m_fired(due->first);
    return true;
}

void FakeHub::fire(const TimerKey key) {
    m_due.remove(code(key));
    if (m_fired)
        m_fired(key);
}

// FakeRelayPort.
void FakeRelayPort::enter(const QString& name) {
    calls.append(name);
    maxDepth = std::max(maxDepth, ++depth);
    QEventLoop nested;
    QTimer::singleShot(spinMs, &nested, &QEventLoop::quit);
    if (during)
        QTimer::singleShot(0, &nested, [this, name] { during(name); });
    nested.exec(QEventLoop::ExcludeUserInputEvents);
}

AccessTokens::Call FakeRelayPort::heartbeat(const QJsonObject& body) {
    enter(QStringLiteral("heartbeat"));
    heartbeats.append(body);
    AccessTokens::Call call;
    if (heartbeatAnswer) {
        call = heartbeatAnswer(body);
    } else {
        call.token.outcome = AccessTokens::Outcome::Ok;
        call.relay.kind = RelayResult::Kind::Ok;
        call.relay.body = {{QStringLiteral("disabled"), false},
                           {QStringLiteral("pending"), pending}};
    }
    leave();
    return call;
}

CallResult FakeRelayPort::lease(const LeaseCall& call) {
    enter(kindName(call.kind));
    leases.append(call);
    const CallResult result = leaseAnswer ? leaseAnswer(call) : ok();
    leave();
    return result;
}

RelayPort::Token FakeRelayPort::token(const bool renew) {
    enter(renew ? QStringLiteral("token renew") : QStringLiteral("token"));
    const Token token =
        tokenAnswer ? tokenAnswer(renew)
                    : Token{AccessTokens::Outcome::Ok, QString::fromLatin1(tokenMarker) + "-1"};
    leave();
    return token;
}

std::optional<ExitCode> FakeRelayPort::pairings() {
    enter(QStringLiteral("pairings"));
    leave();
    return pairingsExit;
}

// FakeSocket, FakeTransports, FakeCommand, FakeReceiver.
void FakeSocket::open(const QString& accessToken) {
    tokens.append(accessToken);
    live = true;
}

bool FakeSocket::send(const QJsonObject& message) {
    if (!live)
        return false;
    sent.append(message);
    if (onSend)
        onSend(message);
    return true;
}

std::unique_ptr<peer::PeerTransport> FakeTransports::create(peer::PeerTransportEvents& events) {
    auto state = std::make_shared<TransportState>();
    state->events = &events;
    made.append(state);
    return std::make_unique<FakeTransport>(state);
}

void FakeCommand::cleanUpLeftovers(std::function<void(bool)> done) {
    if (order)
        order->append(QStringLiteral("cleanup"));
    if (autoCleanup)
        done(*autoCleanup);
    else
        m_cleanupDone = std::move(done);
}

void FakeCommand::finishCleanup(const bool clean) {
    if (m_cleanupDone)
        std::exchange(m_cleanupDone, {})(clean);
}

void FakeCommand::launch(const session::LaunchRequest& request) { launches.append(request); }

void FakeCommand::set(const Readiness next) {
    state = next;
    if (m_changed)
        m_changed();
}

void FakeReceiver::begin(const protocol::JobSpec& spec, const QString&, Send) {
    begun.append(spec);
}

// Run.
Run::Run() : root(QStringLiteral("/tmp/ketplus-run-XXXXXX")) {
    hub.start = QDateTime(QDate(2026, 11, 18), QTime(3, 0), QTimeZone::UTC);
    QString error;
    state = StateDirectory::open(QFileInfo(root.path()).canonicalFilePath() + "/state", &error);
    if (!state)
        qFatal("state directory: %s", qPrintable(error));
    runnerState = std::make_unique<RunnerState>(*state);
    pairing::RunnerPin pin;
    pin.machineId = QString::fromLatin1(vectors::machineId);
    pin.workspaceId = QString::fromLatin1(vectors::workspaceId);
    pin.machineKeyVersion = 1;
    pin.signingPublicKey = QByteArray::fromHex(vectors::desktopPublicKey);
    pin.agreementPublicKey = QByteArray(32, '\x11');
    pin.desktopFingerprint =
        pairing::desktopFingerprint(pin.signingPublicKey, pin.agreementPublicKey);
    pin.state = pairing::RunnerPin::State::Active;
    pin.pairedAt = hub.start;
    if (!runnerState->savePin(pin, &error))
        qFatal("pin: %s", qPrintable(error));

    identity.privateKey = QByteArray::fromHex(vectors::runnerSeed);
    identity.credential.runnerId = QString::fromLatin1(vectors::runnerId);
    identity.credential.keyVersion = 1;
    identity.config.labels = {QStringLiteral("gpu")};

    environment.now = [this] { return hub.now(); };
    environment.sleep = [](qint64) { qFatal("the run loop never sleeps"); };
    environment.stopRequested = [] { return false; };
    environment.random = [] { return 0.0; };
    environment.out = [this](const QString& line) { lines.append(line); };
    environment.err = [this](const QString& line) { lines.append(line); };
    environment.readStdin = [] { return QByteArray(); };
    environment.variable = [](const char*) { return QString(); };
    environment.platform = {QStringLiteral("linux"), QStringLiteral("x86_64")};
    environment.runnerVersion = QStringLiteral("0.1.0-test");

    signer = std::make_unique<IdentitySigner>(identity);
    pins = std::make_unique<StatePins>(*runnerState);
    trust = std::make_unique<PinTrust>(*runnerState);
    journal = std::make_unique<JobJournalFile>(*state);
    accepted = AcceptedJobsFile::load(*state, environment.now, &acceptedError);

    relay.leaseAnswer = [this](const LeaseCall& call) {
        switch (call.kind) {
        case LeaseCall::Kind::Acquire:
            return ok(leaseReply(call.jobId.isEmpty() ? QStringLiteral("job_42") : call.jobId,
                                 QStringLiteral("lease_%1").arg(++acquireCount)));
        case LeaseCall::Kind::Start:
            return ok({{QStringLiteral("startedAt"), QStringLiteral("2026-11-18T03:00:01.000Z")}});
        default:
            return ok();
        }
    };
}

Run::~Run() {
    loop.reset();
    QCoreApplication::processEvents();
}

void Run::make(const ReceiverHost* host) {
    session::RunnerInfo info;
    info.runnerId = identity.credential.runnerId;
    info.runnerVersion = environment.runnerVersion;
    info.os = QStringLiteral("linux");
    info.arch = QStringLiteral("x86_64");
    info.labels = delivery::effectiveLabels(identity.config.labels, info.arch);
    info.baseEnvironment = {{QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin")}};
    ReceiverHost fake{receiver,
                      [this](auto ready, auto failed) {
                          receiver.ready = std::move(ready);
                          receiver.failed = std::move(failed);
                      },
                      [this] { ++receiver.idles; }};
    session::AcceptedJobs& jobs = *accepted;
    loop = std::make_unique<RunnerLoop>(RunnerDeps{
        environment,
        relay,
        [this](peer::RelaySocketEvents&) {
            auto made = std::make_unique<FakeSocket>();
            socket = made.get();
            return made;
        },
        realTransports ? *realTransports : static_cast<peer::PeerTransportFactory&>(transports),
        *signer,
        *pins,
        *trust,
        *journal,
        jobs,
        acceptedError,
        host ? *host : fake,
        command,
        hub,
        info,
        identity.config.labels,
        [n = 0]() mutable { return QStringLiteral("acq_%1").arg(++n); },
        [this] { return stops; },
    });
}

void Run::start() {
    if (!loop)
        make();
    loop->start([this](const ExitCode code) { exit = code; });
    settle();
}

void Run::settle() {
    for (int round = 0; round < 1000; ++round) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        if (!loop || loop->queue().idle())
            break;
    }
}

void Run::advance(const qint64 ms) {
    const qint64 target = hub.elapsed + ms;
    while (const auto due = hub.next()) {
        if (due->second > target)
            break;
        hub.fireNext();
        settle();
    }
    hub.elapsed = target;
    settle();
}

void Run::socketReady() {
    advance(0);
    if (!socket || !socket->live)
        qFatal("the Relay socket was not opened");
    loop->socketOpened();
    loop->socketText(QStringLiteral(R"({"type":"ready","role":"runner"})"));
    settle();
}

void Run::wakeup(const QJsonObject& message) {
    loop->socketText(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    settle();
}

void Run::acquire(const QString& jobId) {
    relay.pending = true;
    relay.leaseAnswer = [this, jobId, previous = relay.leaseAnswer](const LeaseCall& call) {
        if (call.kind == LeaseCall::Kind::Acquire)
            return ok(leaseReply(jobId, QStringLiteral("lease_%1").arg(++acquireCount)));
        return previous(call);
    };
    advance(heartbeatIntervalMs);
    relay.pending = false;
}

void Run::connectDesktop() {
    const auto start = std::find_if(socket->sent.cbegin(), socket->sent.cend(), [](const auto& m) {
        return m.value(QStringLiteral("type")).toString() == QStringLiteral("signal.start");
    });
    if (start == socket->sent.cend())
        qFatal("no signal.start");
    const QString jobId = start->value(QStringLiteral("jobId")).toString();
    wakeup({{QStringLiteral("type"), QStringLiteral("signal.started")},
            {QStringLiteral("connectionId"), connectionId},
            {QStringLiteral("iceServers"), QJsonArray{}}});
    TransportState& transport = *transports.made.last();
    transport.events->localDescription(offerSdp);
    settle();
    wakeup(signedAnswer(connectionId, jobId, hub.now()));
    transport.events->opened();
    settle();
    deliver(protocol::Welcome{
        connectionId, 7, {QString(protocol::capability::runnerProtocol)}, QStringLiteral("9.9.9")});
}

void Run::deliver(const protocol::Frame& frame) {
    const protocol::EncodeResult encoded =
        protocol::encode(frame, protocol::Direction::DesktopToRunner);
    if (!encoded.ok())
        qFatal("a test frame breaks a rule");
    const protocol::Route* route = protocol::routeFor(protocol::frameType(frame));
    const peer::Channel channel = route->channel == protocol::Channel::Control
                                      ? peer::Channel::Control
                                      : peer::Channel::Events;
    transports.made.last()->events->received(channel, encoded.message, false);
    settle();
}

QList<protocol::Frame> Run::sentFrames() const {
    QList<protocol::Frame> frames;
    for (const auto& transport : transports.made) {
        for (const auto& [channel, bytes, binary] : transport->sent) {
            if (binary)
                continue;
            const auto decoded =
                protocol::decode(bytes,
                                 channel == peer::Channel::Control ? protocol::Channel::Control
                                                                   : protocol::Channel::Events,
                                 protocol::Direction::RunnerToDesktop);
            if (decoded.ok())
                frames.append(*decoded.frame);
        }
    }
    return frames;
}

QStringList Run::sentTypes() const {
    QStringList types;
    for (const protocol::Frame& frame : sentFrames())
        types.append(QString(protocol::frameType(frame)));
    return types;
}

QStringList Run::leaseKinds() const {
    QStringList kinds;
    for (const LeaseCall& call : relay.leases) {
        QString kind = kindName(call.kind);
        if (!call.reason.isEmpty())
            kind += QLatin1Char(' ') + (call.outcome.isEmpty() ? QString() : call.outcome + ' ') +
                    call.reason;
        kinds.append(kind);
    }
    return kinds;
}

std::optional<delivery::JournalRecord> Run::journalRecord() const { return journal->load().record; }

QString Run::heartbeatState(const qsizetype index) const {
    if (relay.heartbeats.isEmpty())
        return {};
    const qsizetype at = index < 0 ? relay.heartbeats.size() + index : index;
    return relay.heartbeats.at(at).value(QStringLiteral("state")).toString();
}

QJsonObject leaseReply(const QString& jobId, const QString& leaseId, const QString& machineId) {
    return {
        {QStringLiteral("jobId"), jobId},
        {QStringLiteral("leaseId"), leaseId},
        {QStringLiteral("attempt"), 1},
        {QStringLiteral("leaseExpiresAt"), QStringLiteral("2026-11-18T03:05:00.000Z")},
        {QStringLiteral("runsOn"), QJsonArray{QStringLiteral("gpu")}},
        {QStringLiteral("requester"),
         QJsonObject{{QStringLiteral("machineId"), machineId},
                     {QStringLiteral("workspaceId"), QString::fromLatin1(vectors::workspaceId)}}}};
}

CallResult ok(const QJsonObject& body) {
    CallResult result;
    result.kind = CallResult::Kind::Ok;
    result.body = body;
    return result;
}

CallResult refused(const QString& code, const QJsonObject& body) {
    CallResult result;
    result.kind = CallResult::Kind::Refused;
    result.code = code;
    result.body = body;
    return result;
}

protocol::JobSpec makeSpec(const QString& jobId) {
    protocol::JobSpec spec;
    spec.epoch = 7;
    spec.jobId = jobId;
    spec.attempt = 1;
    spec.invocation.argv = QStringList{QStringLiteral("make"), QString::fromLatin1(commandMarker)};
    spec.env = {{QStringLiteral("SECRET_VALUE"), QString::fromLatin1(commandMarker)}};
    spec.timeoutMs = 600'000;
    spec.repositoryKey = QString(32, QLatin1Char('r'));
    spec.workspaceKey = QString(32, QLatin1Char('w'));
    spec.snapshotId = QString(40, QLatin1Char('a'));
    spec.sourceHead = QString(40, QLatin1Char('b'));
    spec.objectFormat = QStringLiteral("sha1");
    return spec;
}

} // namespace runner_run_test

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    int failures = 0;
    {
        runner_run_test::RunnerRunStartTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_run_test::RunnerRunJobTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_run_test::RunnerRunStopTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    return failures == 0 ? 0 : 1;
}
