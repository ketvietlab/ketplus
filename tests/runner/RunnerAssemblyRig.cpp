#include "RunnerAssemblyRig.h"

#include "cloud/CryptoPrimitives.h"
#include "commands/Commands.h"
#include "pairing/PairingSignatures.h"
#include "protocol/Capabilities.h"
#include "protocol/FrameLimits.h"
#include "protocol/FrameRoutes.h"
#include "relay/RelayClient.h"
#include "run/AccessTokens.h"
#include "run/RunnerAssembly.h"
#include "run/RunnerLoop.h"
#include "snapshot/WorktreeSnapshot.h"
#include "state/RunnerState.h"
#include "state/StateDirectory.h"
#include "transport/FrameCodec.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <QTest>
#include <QTimer>

#include <signal.h>

namespace runner_assembly_test {

namespace cc = ketplus::control_crypto;

namespace {

constexpr qint64 epoch = 7;
const QString connectionId = QStringLiteral("pc_assemblyassemblyassembly");
const QString offerSdp = QStringLiteral("v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\n");

QString answerSdp() {
    QStringList pairs;
    for (int i = 0; i < 32; ++i)
        pairs.append(QStringLiteral("%1").arg(i * 7 % 256, 2, 16, QLatin1Char('0')).toUpper());
    return QStringLiteral("v=0\r\no=- 3 4 IN IP4 127.0.0.1\r\na=fingerprint:sha-256 ") +
           pairs.join(QLatin1Char(':')) + QStringLiteral("\r\n");
}

QString text(const QJsonObject& message) {
    return QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact));
}

class PeerTransport final : public peer::PeerTransport {
  public:
    explicit PeerTransport(std::shared_ptr<Peer> peer) : m_peer(std::move(peer)) {}
    ~PeerTransport() override { m_peer->alive = false; }
    bool offer(const QJsonArray&) override { return true; }
    bool acceptAnswer(const QString&, const QString&) override { return true; }
    bool candidate(const QString&, const QString&) override { return true; }
    bool send(const peer::Channel channel, const QByteArray& bytes, const bool binary) override {
        m_peer->sent.append({channel, bytes, binary});
        return true;
    }
    [[nodiscard]] quint64 buffered(peer::Channel) const override { return 0; }
    void close() override { m_peer->alive = false; }

  private:
    std::shared_ptr<Peer> m_peer;
};

class Transports final : public peer::PeerTransportFactory {
  public:
    explicit Transports(QList<std::shared_ptr<Peer>>& peers) : m_peers(peers) {}
    std::unique_ptr<peer::PeerTransport> create(peer::PeerTransportEvents& events) override {
        auto made = std::make_shared<Peer>();
        made->events = &events;
        m_peers.append(made);
        return std::make_unique<PeerTransport>(made);
    }

  private:
    QList<std::shared_ptr<Peer>>& m_peers;
};

} // namespace

class TestSocket final : public peer::RelaySocket {
  public:
    explicit TestSocket(peer::RelaySocketEvents& events) : m_events(events) {}
    void open(const QString&) override {
        QTimer::singleShot(0, &m_home, [this] {
            m_events.socketOpened();
            m_events.socketText(QStringLiteral(R"({"type":"ready","role":"runner"})"));
        });
    }
    bool send(const QJsonObject& message) override {
        sent.append(message);
        return true;
    }
    void close() override {}
    void answer(const QJsonObject& message) { m_events.socketText(text(message)); }

    QList<QJsonObject> sent;

  private:
    peer::RelaySocketEvents& m_events;
    QObject m_home;
};

struct Rig::Running {
    std::optional<app::StateDirectory> directory;
    std::unique_ptr<app::RunnerState> state;
    std::optional<app::RunnerIdentity> identity;
    std::unique_ptr<app::RelayClient> client;
    std::unique_ptr<app::AccessTokens> tokens;
    std::unique_ptr<app::PairingContext> context;
    std::unique_ptr<app::RunnerAssembly> assembly;
};

QStringList processesHolding(const QByteArray& marker) {
    QStringList found;
    for (const QString& entry : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs)) {
        bool number = false;
        (void)entry.toLongLong(&number);
        QFile file(QStringLiteral("/proc/%1/cmdline").arg(entry));
        if (!number || !file.open(QIODevice::ReadOnly))
            continue;
        QByteArray line = file.readAll();
        if (line.contains(marker))
            found.append(entry + u' ' + QString::fromUtf8(line.replace('\0', ' ')).left(160));
    }
    return found;
}

int processesWith(const QByteArray& marker) { return int(processesHolding(marker).size()); }

// The ketplus-job-* unit whose cgroup holds the process, from /proc/<pid>/cgroup; empty if none.
static QString jobUnitOf(const QString& pid) {
    QFile file(QStringLiteral("/proc/%1/cgroup").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    for (const QByteArray& line : file.readAll().split('\n')) {
        // hierarchy-id:controllers:path; the path may hold ':' itself.
        const qsizetype second = line.indexOf(':', line.indexOf(':') + 1);
        if (second < 0)
            continue;
        for (const QByteArray& part : line.mid(second + 1).split('/'))
            if (part.startsWith("ketplus-job-"))
                return QString::fromUtf8(part);
    }
    return {};
}

void removeLeftovers(const QByteArray& marker) {
    // Only the units that hold this test's processes: other suites on the machine keep theirs.
    const QStringList found = processesHolding(marker);
    QSet<QString> units;
    for (const QString& process : found)
        if (const QString unit = jobUnitOf(process.section(u' ', 0, 0)); !unit.isEmpty())
            units.insert(unit);
    for (const QString& unit : units)
        QProcess::execute(QStringLiteral("systemctl"),
                          {QStringLiteral("--user"), QStringLiteral("stop"), unit});
    for (const QString& process : processesHolding(marker))
        ::kill(pid_t(process.section(u' ', 0, 0).toLongLong()), SIGKILL);
}

Desktop desktopOf(FakeRelay& relay) {
    const FakeRelay::Desktop& keys = relay.desktop(QStringLiteral("desktop-1"));
    return {keys.machineId, relay.workspaceId, keys.keyVersion, keys.signingPrivateKey,
            relay.lastRunnerId};
}

Rig::Rig(const QString& root, const QUrl& relay)
    : m_root(root), m_state(root + QStringLiteral("/state")),
      m_work(root + QStringLiteral("/work")), m_transports(std::make_unique<Transports>(m_peers)) {
    m_environment.now = [] { return QDateTime::currentDateTimeUtc(); };
    m_environment.sleep = [](const qint64 ms) { QTest::qWait(int(ms)); };
    m_environment.stopRequested = [] { return false; };
    m_environment.random = [] { return 0.0; };
    m_environment.out = [this](const QString& line) { m_lines.append(line); };
    m_environment.err = [this](const QString& line) { m_lines.append(line); };
    m_environment.readStdin = [] { return QByteArray(); };
    // The session's runtime directory, PATH and HOME are the machine's.
    m_environment.variable = [](const char* name) { return qEnvironmentVariable(name); };
    m_environment.platform = {QStringLiteral("linux"), QStringLiteral("x86_64")};
    m_environment.hostName = QStringLiteral("build-node-1");
    m_environment.relayUrl = relay;
    m_environment.runnerVersion = QStringLiteral("0.1.0-test");
    m_environment.requestTimeoutMs = 5'000;
}

Rig::~Rig() {
    if (m_running && !m_exit)
        (void)stop(60'000);
    m_running.reset();
    QCoreApplication::processEvents();
}

bool Rig::registerRunner(FakeRelay& relay) {
    const QByteArray token = relay.createToken(QStringLiteral("desktop-1"));
    const int code = app::runCli({QStringLiteral("register"), QString::fromLatin1(token),
                                  QStringLiteral("--state-dir"), m_state},
                                 m_environment);
    QDir().mkpath(m_work);
    return code == 0 &&
           QFile::setPermissions(m_work, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

bool Rig::start() {
    m_running = std::make_unique<Running>();
    Running& r = *m_running;
    QString error;
    r.directory = app::StateDirectory::open(m_state, &error);
    if (!r.directory)
        return false;
    r.state = std::make_unique<app::RunnerState>(*r.directory);
    r.identity = r.state->loadIdentity(&error);
    if (!r.identity)
        return false;
    r.client =
        std::make_unique<app::RelayClient>(m_environment.relayUrl, m_environment.requestTimeoutMs);
    r.tokens = std::make_unique<app::AccessTokens>(*r.client, *r.state, *r.identity, m_environment);
    r.context = std::make_unique<app::PairingContext>(
        app::PairingContext{m_environment, *r.client, *r.state, *r.tokens});
    app::ExitCode failure = app::ExitCode::Failure;
    r.assembly =
        app::RunnerAssembly::create(*r.context, *r.identity, *r.directory, m_work,
                                    app::RunSeams{m_timers,
                                                  [this](peer::RelaySocketEvents& events) {
                                                      auto made =
                                                          std::make_unique<TestSocket>(events);
                                                      m_socket = made.get();
                                                      return made;
                                                  },
                                                  *m_transports, [this] { return m_stops; }},
                                    &failure);
    if (!r.assembly)
        return false;
    r.assembly->loop().start([this](const app::ExitCode code) { m_exit = int(code); });
    return true;
}

bool Rig::stop(const int timeoutMs) {
    // As SIGTERM would: the loop reads the count every 250 ms.
    ++m_stops;
    return QTest::qWaitFor([this] { return m_exit.has_value(); }, timeoutMs);
}

QString Rig::repository(const QMap<QString, QByteArray>& files) const {
    const QString path = m_root + QStringLiteral("/repo");
    QDir().mkpath(path);
    for (auto file = files.cbegin(); file != files.cend(); ++file) {
        QDir().mkpath(QFileInfo(path + u'/' + file.key()).absolutePath());
        QFile out(path + u'/' + file.key());
        if (!out.open(QIODevice::WriteOnly) || out.write(file.value()) != file.value().size())
            return {};
    }
    QProcess git;
    git.setWorkingDirectory(path);
    git.start(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("-q")});
    return git.waitForFinished(60'000) && git.exitCode() == 0 ? path : QString();
}

bool Rig::runDesktop(const Desktop& desktop, const QString& repo, protocol::JobSpec spec) {
    const auto failed = [this](const QString& step) {
        m_failedStep = step;
        return false;
    };
    const auto signalled = [this](const QString& type) -> std::optional<QJsonObject> {
        if (m_socket)
            for (const QJsonObject& message : m_socket->sent)
                if (message.value(QStringLiteral("type")).toString() == type)
                    return message;
        return std::nullopt;
    };
    if (!QTest::qWaitFor([&] { return signalled(QStringLiteral("signal.start")).has_value(); },
                         120'000))
        return failed(QStringLiteral("signal.start"));
    const QJsonObject start = *signalled(QStringLiteral("signal.start"));
    spec.jobId = start.value(QStringLiteral("jobId")).toString();
    m_socket->answer({{QStringLiteral("type"), QStringLiteral("signal.started")},
                      {QStringLiteral("connectionId"), connectionId},
                      {QStringLiteral("iceServers"), QJsonArray{}}});
    if (!QTest::qWaitFor([this] { return !m_peers.isEmpty(); }, 10'000))
        return failed(QStringLiteral("signal.started"));
    Peer& transport = *m_peers.last();
    transport.events->localDescription(offerSdp);
    if (!QTest::qWaitFor([&] { return signalled(QStringLiteral("signal.offer")).has_value(); },
                         10'000))
        return failed(QStringLiteral("signal.offer"));

    // The answer, signed by the desktop the runner paired with.
    pairing::AnswerPayload payload;
    payload.connectionId = connectionId;
    payload.workspaceId = desktop.workspaceId;
    payload.machineId = desktop.machineId;
    payload.machineKeyVersion = desktop.keyVersion;
    payload.runnerId = desktop.runnerId;
    payload.jobId = spec.jobId;
    payload.offerSdp = offerSdp;
    payload.issuedAt = pairing::issuedAtString(QDateTime::currentDateTimeUtc());
    payload.sdp = answerSdp();
    const QByteArray signature = pairing::signPayload(desktop.signingPrivateKey, payload.bytes());
    m_socket->answer(
        {{QStringLiteral("type"), QStringLiteral("signal.answer")},
         {QStringLiteral("connectionId"), connectionId},
         {QStringLiteral("sdp"), payload.sdp},
         {QStringLiteral("machineKeyVersion"), desktop.keyVersion},
         {QStringLiteral("issuedAt"), payload.issuedAt},
         {QStringLiteral("signature"), QString::fromLatin1(cc::base64Url(signature))}});
    QCoreApplication::processEvents();
    transport.events->opened();
    QCoreApplication::processEvents();
    deliver(protocol::Welcome{connectionId,
                              epoch,
                              {QString(protocol::capability::runnerProtocol)},
                              QStringLiteral("9.9.9")});

    // A real snapshot of the repository, sent whole.
    const auto made = snapshot::createSnapshot({repo, m_root + QStringLiteral("/desktop-store")});
    if (!made)
        return failed(QStringLiteral("snapshot"));
    const QString commit = made.value().commit;
    const QString packPath = m_root + QStringLiteral("/desktop-pack");
    if (!snapshot::writePack({m_root + QStringLiteral("/desktop-store"), commit, {}, packPath}))
        return failed(QStringLiteral("pack"));
    QFile packFile(packPath);
    if (!packFile.open(QIODevice::ReadOnly))
        return failed(QStringLiteral("pack read"));
    const QByteArray pack = packFile.readAll();
    spec.epoch = epoch;
    spec.snapshotId = commit;
    spec.sourceHead = commit;
    deliver(spec);
    protocol::SnapshotOffer offer;
    offer.epoch = epoch;
    offer.jobId = spec.jobId;
    offer.snapshotId = commit;
    offer.packBytes = pack.size();
    offer.packSha256 =
        QString::fromLatin1(QCryptographicHash::hash(pack, QCryptographicHash::Sha256).toHex());
    offer.chunkBytes = protocol::limits::chunkBytes;
    deliver(offer);
    const auto wanted = [this] {
        for (const protocol::Frame& frame : sentFrames())
            if (std::holds_alternative<protocol::SnapshotWant>(frame))
                return true;
        return false;
    };
    if (!QTest::qWaitFor(wanted, 30'000))
        return failed(QStringLiteral("snapshot.want"));
    protocol::SnapshotChunk chunk;
    chunk.epoch = epoch;
    chunk.snapshotId = commit;
    chunk.offset = 0;
    chunk.bytes = pack.size();
    chunk.sha256 = offer.packSha256;
    chunk.transferId = QStringLiteral("000000000000001f");
    deliver(chunk);
    for (const auto& packet : ketplus::daemon::transport::FrameCodec::encodeTransfer(0x1f, pack))
        transport.events->received(peer::Channel::Bulk, packet.bytes, true);
    QCoreApplication::processEvents();
    return true;
}

void Rig::deliver(const protocol::Frame& frame) {
    const protocol::EncodeResult encoded =
        protocol::encode(frame, protocol::Direction::DesktopToRunner);
    if (!encoded.ok())
        qFatal("a test frame breaks a rule");
    const protocol::Route* route = protocol::routeFor(protocol::frameType(frame));
    const peer::Channel channel = route->channel == protocol::Channel::Control
                                      ? peer::Channel::Control
                                      : peer::Channel::Events;
    m_peers.last()->events->received(channel, encoded.message, false);
    QCoreApplication::processEvents();
}

QList<protocol::Frame> Rig::sentFrames() const {
    QList<protocol::Frame> frames;
    for (const auto& transport : m_peers)
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
    return frames;
}

std::optional<protocol::JobResult> Rig::result() const {
    for (const protocol::Frame& frame : sentFrames())
        if (const auto* result = std::get_if<protocol::JobResult>(&frame))
            return *result;
    return std::nullopt;
}

QByteArray Rig::output() const {
    QByteArray bytes;
    for (const protocol::Frame& frame : sentFrames())
        if (const auto* chunk = std::get_if<protocol::JobOutput>(&frame))
            bytes += chunk->data;
    return bytes;
}

void Rig::acknowledge(const QString& jobId) { deliver(protocol::JobResultAck{epoch, jobId}); }

} // namespace runner_assembly_test
