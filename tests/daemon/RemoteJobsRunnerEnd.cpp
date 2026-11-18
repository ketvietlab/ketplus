#include "RemoteJobsRunnerEnd.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingSignatures.h"
#include "protocol/Capabilities.h"
#include "protocol/FrameRoutes.h"
#include "runner/output/TaskOutputSpool.h"
#include "snapshot/UserGitRules.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>

namespace remote_jobs_test {

namespace {

namespace pairing = ketplus::runner::pairing;

// The fixed test keys of the pairing tests.
constexpr const char* runnerSeed =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
constexpr const char* runnerPublicKey =
    "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8";
constexpr const char* desktopSeed =
    "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f";
const QString workspace = QStringLiteral("ws_alpha");
const QString machine = QStringLiteral("m_desktop01");
const QString answerSdp = QStringLiteral("v=0\r\no=- 3 4 IN IP4 127.0.0.1\r\n");
const QString offerSdp = QStringLiteral("v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\n");

class TestTransport final : public link::LinkTransport {
  public:
    explicit TestTransport(std::shared_ptr<Wire> wire) : wire_(std::move(wire)) {}
    ~TestTransport() override { wire_->alive = false; }
    bool answer(const QString&, const QJsonArray&) override { return true; }
    bool candidate(const QString&, const QString&) override { return true; }
    bool send(const link::TransportChannel channel, const QByteArray& bytes,
              const bool binary) override {
        wire_->sent.append({channel, bytes, binary});
        return true;
    }
    [[nodiscard]] quint64 buffered(link::TransportChannel) const override { return 0; }
    void close() override { wire_->closed = true; }

  private:
    std::shared_ptr<Wire> wire_;
};

transport::Channel transportChannel(const protocol::Channel channel) {
    switch (channel) {
    case protocol::Channel::Control:
        return transport::Channel::Control;
    case protocol::Channel::Events:
        return transport::Channel::Events;
    case protocol::Channel::Bulk:
        break;
    }
    return transport::Channel::Bulk;
}

} // namespace

std::unique_ptr<link::LinkTransport> TestTransports::create(link::LinkTransportEvents& events) {
    auto wire = std::make_shared<Wire>();
    wire->events = &events;
    wires.append(wire);
    return std::make_unique<TestTransport>(wire);
}

QString TestIdentity::workspaceId() const { return workspace; }
QString TestIdentity::machineId() const { return machine; }

QByteArray TestIdentity::sign(const QByteArray& bytes) const {
    return pairing::signPayload(QByteArray::fromHex(desktopSeed), bytes);
}

std::optional<pairing::DesktopRunnerPin> TestPins::pin(const QString& runnerId) const {
    pairing::DesktopRunnerPin pin;
    pin.runnerId = runnerId;
    pin.runnerKeyVersion = 1;
    pin.publicKey = QByteArray::fromHex(runnerPublicKey);
    pin.name = QStringLiteral("builder");
    pin.pairedAt = QDateTime(QDate(2026, 11, 1), QTime(9, 0), QTimeZone::UTC);
    return pin;
}

int TestOut::count(const QString& type) const {
    return static_cast<int>(std::count_if(sent.cbegin(), sent.cend(), [&](const QJsonObject& m) {
        return m.value(QStringLiteral("type")).toString() == type;
    }));
}

QString utc(const QDateTime& time) { return time.toUTC().toString(Qt::ISODateWithMs); }

QJsonObject signedOffer(const QString& connectionId, const QString& jobId, const qint64 attempt,
                        const QDateTime& issuedAt) {
    const QString issued = utc(issuedAt);
    const pairing::OfferPayload payload{connectionId, workspace,    machine, linkedRunner, 1,
                                        jobId,        int(attempt), issued,  offerSdp};
    const QByteArray signature =
        pairing::signPayload(QByteArray::fromHex(runnerSeed), payload.bytes());
    return {{QStringLiteral("type"), QStringLiteral("signal.offer")},
            {QStringLiteral("connectionId"), connectionId},
            {QStringLiteral("sdp"), offerSdp},
            {QStringLiteral("jobId"), jobId},
            {QStringLiteral("attempt"), attempt},
            {QStringLiteral("runnerKeyVersion"), 1},
            {QStringLiteral("issuedAt"), issued},
            {QStringLiteral("signature"),
             QString::fromLatin1(ketplus::control_crypto::base64Url(signature))},
            {QStringLiteral("runnerId"), linkedRunner},
            {QStringLiteral("iceServers"), QJsonArray{}}};
}

QString linkConnection(const int n) {
    return QStringLiteral("pc_") + QString::number(n).rightJustified(24, QLatin1Char('0'));
}

void RunnerEnd::open() {
    wire_->events->localDescription(answerSdp);
    wire_->events->opened();
}

QList<protocol::Frame> RunnerEnd::take() {
    QList<protocol::Frame> frames;
    while (read_ < wire_->sent.size()) {
        const transport::Packet packet = wire_->sent.at(read_++);
        if (packet.channel == transport::Channel::Bulk) {
            QString error;
            if (const auto done = codec_.receiveTransfer(packet, 0, &error))
                transfers_.insert(done->transferId, done->bytes);
            continue;
        }
        const protocol::Channel channel = packet.channel == transport::Channel::Control
                                              ? protocol::Channel::Control
                                              : protocol::Channel::Events;
        const protocol::DecodeResult decoded =
            protocol::decode(packet.bytes, channel, protocol::Direction::DesktopToRunner);
        if (decoded.ok())
            frames.append(*decoded.frame);
        else
            qWarning() << "runner end: undecodable frame" << decoded.error.type;
    }
    return frames;
}

void RunnerEnd::send(const protocol::Frame& frame) {
    const protocol::EncodeResult encoded =
        protocol::encode(frame, protocol::Direction::RunnerToDesktop);
    if (!encoded.ok()) {
        qWarning() << "runner end: unencodable frame" << protocol::frameType(frame);
        return;
    }
    const protocol::Route* route = protocol::routeFor(protocol::frameType(frame));
    wire_->events->received(transportChannel(route->channel), encoded.message, false);
}

void RunnerEnd::lose() { wire_->events->closed(); }

void LinkedParts::attach(RemoteHarness& h) {
    h.adjust = [this, &h](RemoteJobsPorts& ports) {
        ports.makeLinks = [this](link::LinkEvents& events) {
            return std::make_unique<link::RunnerLinks>(out, clock, transports, identity, pins,
                                                       events);
        };
        ports.openSpool = [this, &h](const QString& taskId)
            -> std::unique_ptr<ketplus::daemon::runner::peer::OutputSpool> {
            if (failSpool)
                return nullptr;
            return ketplus::daemon::runner::output::TaskOutputSpool::open(h.f.database, taskId);
        };
    };
    h.trust.pins.insert(linkedRunner, QStringLiteral("builder"));
    h.build();
}

protocol::Hello helloFor(const QString& connectionId, const QString& jobId, const qint64 attempt) {
    protocol::Hello hello;
    hello.connectionId = connectionId;
    hello.capabilities = {QString(protocol::capability::runnerProtocol)};
    hello.runnerId = linkedRunner;
    hello.runnerVersion = QStringLiteral("0.1.0");
    hello.os = QStringLiteral("linux");
    hello.arch = QStringLiteral("x86_64");
    hello.labels = linuxLabels;
    hello.jobId = jobId;
    hello.attempt = attempt;
    return hello;
}

namespace {

bool git(const QString& home, const QString& directory, const QStringList& arguments) {
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("PATH"), qEnvironmentVariable("PATH"));
    environment.insert(QStringLiteral("HOME"), home);
    environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
    environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QDir(home).filePath(".gitconfig"));
    for (const auto* name : {"GIT_AUTHOR_NAME", "GIT_COMMITTER_NAME"})
        environment.insert(QString::fromLatin1(name), QStringLiteral("Fixture"));
    for (const auto* name : {"GIT_AUTHOR_EMAIL", "GIT_COMMITTER_EMAIL"})
        environment.insert(QString::fromLatin1(name), QStringLiteral("fixture@example.invalid"));
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory);
    process.start(QStringLiteral("git"), arguments);
    return process.waitForFinished(60'000) && process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
}

bool writeFile(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(content) == content.size();
}

} // namespace

EndToEnd::EndToEnd() {
    parts.attach(h);
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    home = QDir(root).filePath(QStringLiteral("home"));
    repo = QDir(root).filePath(QStringLiteral("repo"));
    QDir().mkpath(home);
    QDir().mkpath(repo);
    const bool made =
        git(home, repo, {QStringLiteral("init"), QStringLiteral("-q")}) &&
        writeFile(QDir(repo).filePath(QStringLiteral("src/main.c")),
                  "int main() { return 0; }\n") &&
        writeFile(QDir(repo).filePath(QStringLiteral("README")), QByteArray(4096, 'r')) &&
        git(home, repo, {QStringLiteral("add"), QStringLiteral(".")}) &&
        git(home, repo, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("one")}) &&
        writeFile(QDir(repo).filePath(QStringLiteral("src/edit.c")), "int edited;\n");
    if (!made)
        qWarning() << "end to end: the repository could not be made";
    h.prepare = [this](sync::PrepareRequest request) {
        request.userGitSources = ketplus::runner::snapshot::UserGitSources{
            home, QDir(home).filePath(QStringLiteral(".config")), {}};
        return sync::prepareSnapshot(request);
    };
}

QString EndToEnd::submit(const qint64 timeoutMs) {
    const QString id = h.f.remoteTask(linuxLabels, timeoutMs);
    if (id.isEmpty())
        return {};
    SubmitRequest request;
    request.spec.invocation.argv = QStringList{QStringLiteral("make")};
    request.spec.cwd = QDir(repo).filePath(QStringLiteral("src"));
    request.runsOn = linuxLabels;
    request.timeoutMs = timeoutMs;
    QString code = QStringLiteral("unanswered");
    h.jobs->submit(id, request, [&code](const QString& answer) { code = answer; });
    if (!code.isEmpty())
        return {};
    remote_job_test::RelayJob queued = remote_job_test::relayJob(QStringLiteral("queued"));
    queued.jobId = linkedJob;
    return h.relay.answer(QStringLiteral("submit"), queued) ? id : QString();
}

void EndToEnd::assign() {
    h.jobs->relayMessage({{QStringLiteral("type"), QStringLiteral("job.changed")},
                          {QStringLiteral("jobId"), linkedJob}});
    remote_job_test::RelayJob assigned =
        remote_job_test::relayJob(QStringLiteral("acquired"), linkedRunner, 1);
    assigned.jobId = linkedJob;
    (void)h.relay.answer(QStringLiteral("get"), assigned);
}

RunnerEnd EndToEnd::connect(const int n, const qint64 attempt) {
    const QString connection = linkConnection(n);
    const qsizetype before = parts.transports.wires.size();
    h.jobs->relayMessage(signedOffer(connection, linkedJob, attempt, parts.clock.time));
    if (parts.transports.wires.size() == before) {
        qWarning() << "end to end: the offer was not taken";
        return RunnerEnd(std::make_shared<Wire>());
    }
    RunnerEnd end(parts.transports.wires.last());
    end.open();
    end.send(helloFor(connection, linkedJob, attempt));
    return end;
}

bool EndToEnd::play(RunnerEnd& end, const std::function<bool()>& until, const bool runs) {
    QList<protocol::SnapshotChunk> chunks;
    qint64 packBytes = -1;
    QElapsedTimer timer;
    timer.start();
    while (!until()) {
        if (timer.elapsed() > 30'000) {
            qWarning() << "end to end: timed out; the runner saw" << seenTypes
                       << "connection closed" << end.closed();
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        h.run();
        for (const protocol::Frame& frame : end.take()) {
            seen.append(frame);
            seenTypes.append(QString(protocol::frameType(frame)));
            if (const auto* spec = std::get_if<protocol::JobSpec>(&frame)) {
                end.send(protocol::JobAccepted{spec->jobId, spec->attempt});
            } else if (std::holds_alternative<protocol::JobResume>(frame)) {
                // As the runner does: where the job is on the new connection.
                if (started)
                    end.send(protocol::JobStarted{linkedJob, utc(parts.clock.time)});
                else
                    end.send(protocol::JobAccepted{linkedJob, 1});
            } else if (const auto* offer = std::get_if<protocol::SnapshotOffer>(&frame)) {
                if (offer->packSha256 != packSha) {
                    pack.clear();
                    packSha = offer->packSha256;
                }
                packBytes = offer->packBytes;
                end.send(
                    protocol::SnapshotWant{offer->snapshotId, QStringLiteral("need"), pack.size()});
            } else if (const auto* chunk = std::get_if<protocol::SnapshotChunk>(&frame)) {
                chunks.append(*chunk);
            } else if (std::holds_alternative<protocol::JobResultAck>(frame)) {
                resultAcked = true;
            }
        }
        while (!chunks.isEmpty()) {
            const protocol::SnapshotChunk chunk = chunks.first();
            const QByteArray bytes = end.transfer(chunk.transferId.toULongLong(nullptr, 16));
            if (bytes.isEmpty())
                break;
            chunks.removeFirst();
            if (chunk.offset != pack.size() || bytes.size() != chunk.bytes)
                return false;
            pack.append(bytes);
            end.send(protocol::SnapshotAck{chunk.snapshotId, pack.size()});
            if (pack.size() != packBytes)
                continue;
            if (QCryptographicHash::hash(pack, QCryptographicHash::Sha256).toHex() !=
                packSha.toLatin1())
                return false;
            end.send(protocol::SnapshotReady{chunk.snapshotId});
            ready = true;
            if (!runs)
                continue;
            const QString now = utc(parts.clock.time);
            end.send(protocol::JobStarted{linkedJob, now});
            started = true;
            end.send(protocol::JobOutput{linkedJob, 0, output});
            protocol::JobResult result;
            result.jobId = linkedJob;
            result.state = QStringLiteral("exited");
            result.exitCode = 0;
            result.cleanupStatus = QStringLiteral("verified");
            result.outputBytes = output.size();
            result.startedAt = now;
            result.endedAt = now;
            end.send(result);
        }
    }
    return true;
}

} // namespace remote_jobs_test
