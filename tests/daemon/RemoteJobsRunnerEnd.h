#pragma once

#include "RemoteJobsHarness.h"
#include "protocol/Frames.h"
#include "runner/link/RunnerLinks.h"
#include "transport/FrameCodec.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>

#include <functional>

#include <memory>

// The runner's end of real runner links, scripted by the test: transports that hand the links'
// packets to the test, a machine key and runner pins of fixed test keys, and offers the runner
// key signs.
namespace remote_jobs_test {

namespace link = ketplus::daemon::runner::link;
namespace protocol = ketplus::runner::protocol;
namespace transport = ketplus::daemon::transport;

inline const QString linkedRunner = QStringLiteral("run_aaaaaaaaaaaa");
inline const QString linkedJob = QStringLiteral("job_42");

// The time the links' clock answers; offers are issued at it.
class TestLinkClock final : public link::LinkClock {
  public:
    [[nodiscard]] QDateTime now() const override { return time; }
    void arm(const QString& connectionId, qint64 delayMs) override {
        due.insert(connectionId, delayMs);
    }
    void disarm(const QString& connectionId) override { due.remove(connectionId); }

    QDateTime time{QDate(2026, 11, 18), QTime(9, 0), QTimeZone::UTC};
    QMap<QString, qint64> due;
};

// One peer connection: what the links sent and how to answer into them.
struct Wire {
    link::LinkTransportEvents* events = nullptr;
    QList<transport::Packet> sent;
    bool closed = false;
    bool alive = true;
};

class TestTransports final : public link::LinkTransportFactory {
  public:
    [[nodiscard]] std::unique_ptr<link::LinkTransport>
    create(link::LinkTransportEvents& events) override;
    QList<std::shared_ptr<Wire>> wires;
};

class TestIdentity final : public link::MachineIdentity {
  public:
    [[nodiscard]] QString workspaceId() const override;
    [[nodiscard]] QString machineId() const override;
    [[nodiscard]] int machineKeyVersion() const override { return 1; }
    [[nodiscard]] QByteArray sign(const QByteArray& bytes) const override;
};

class TestPins final : public link::RunnerPins {
  public:
    [[nodiscard]] std::optional<ketplus::runner::pairing::DesktopRunnerPin>
    pin(const QString& runnerId) const override;
};

class TestOut final : public link::SignalOut {
  public:
    void sendSignal(const QJsonObject& message) override { sent.append(message); }
    [[nodiscard]] int count(const QString& type) const;
    QList<QJsonObject> sent;
};

// `signal.offer` as the Relay forwards it, signed by the runner key at `issuedAt`.
QJsonObject signedOffer(const QString& connectionId, const QString& jobId, qint64 attempt,
                        const QDateTime& issuedAt);
// `pc_` and 24 characters ending in `n`.
QString linkConnection(int n);

// The runner's side of one connection: decodes what the desktop sent, sends frames back.
class RunnerEnd {
  public:
    explicit RunnerEnd(std::shared_ptr<Wire> wire) : wire_(std::move(wire)) {}
    // The answer is described and the three channels open.
    void open();
    // Frames and completed transfers the desktop sent since the last call.
    QList<protocol::Frame> take();
    [[nodiscard]] QByteArray transfer(quint64 id) const { return transfers_.value(id); }
    void send(const protocol::Frame& frame);
    // The transport closed under the desktop, as ICE or DTLS would.
    void lose();
    [[nodiscard]] bool closed() const { return wire_->closed; }

  private:
    std::shared_ptr<Wire> wire_;
    qsizetype read_ = 0;
    transport::FrameCodec codec_;
    QMap<quint64, QByteArray> transfers_;
};

// The parts a RemoteHarness needs for real links and output spools.
struct LinkedParts {
    TestLinkClock clock;
    TestTransports transports;
    TestIdentity identity;
    TestPins pins;
    TestOut out;
    bool failSpool = false;
    // Hands RemoteJobs real links over these parts and real spools of the harness's registry.
    void attach(RemoteHarness& h);
};

// A real repository in a temporary directory, real links, spools and snapshot preparation, a
// Relay and a runner the test plays.
struct EndToEnd {
    EndToEnd();
    // A task submitted from `repo/src`, its Relay submit answered queued as linkedJob; empty
    // on failure.
    QString submit();
    // The Relay hints a change and answers that linkedRunner took the job.
    void assign();
    // A signed offer of linkedRunner on connection `n`, its channels opened and `hello` sent.
    RunnerEnd connect(int n, qint64 attempt = 1);
    // Plays the runner until `until` holds or the time runs out: accepts the spec, answers
    // `job.resume` as the runner does, wants and acknowledges the snapshot, then runs and sends
    // `output` and an exit code of 0 when `runs` is set. Pumps the event loop and the harness's
    // deferred work.
    bool play(RunnerEnd& end, const std::function<bool()>& until, bool runs = true);

    RemoteHarness h;
    LinkedParts parts;
    QTemporaryDir dir;
    QString home;
    QString repo;
    QByteArray output = QByteArrayLiteral("built\n");
    // What the runner end saw, over every connection.
    QList<protocol::Frame> seen;
    QStringList seenTypes;
    QByteArray pack;
    QString packSha;
    bool ready = false; // the runner sent snapshot.ready
    bool started = false;
    bool resultAcked = false;
};

[[nodiscard]] protocol::Hello helloFor(const QString& connectionId, const QString& jobId,
                                       qint64 attempt = 1);
[[nodiscard]] QString utc(const QDateTime& time);

} // namespace remote_jobs_test
