#pragma once

#include "Environment.h"
#include "FakeRelay.h"
#include "peer/PeerPorts.h"
#include "protocol/Frames.h"
#include "run/RunClocks.h"

#include <QJsonObject>
#include <QList>
#include <QStringList>

#include <memory>
#include <optional>

namespace ketplus::runner::app {
class RunnerAssembly;
}

// `run` through RunnerAssembly::create on Linux, as the runner's main builds it: the Linux host,
// policy and job mounts, the real job executor next to this binary, the journal, the accepted
// jobs and the snapshot store in a state and work directory of the test. Around it: the fake
// Relay over HTTP on loopback, a Relay socket and a peer transport the test drives as the paired
// desktop, and real time.
namespace runner_assembly_test {

using namespace ketplus::runner;
using runner_app_test::FakeRelay;

// The socket the assembly opens: it says ready at once, keeps what the runner sends and lets the
// test answer as the Relay would.
class TestSocket;
// The peer transport of the job's connection, driven by the test as the desktop's end.
struct Peer {
    peer::PeerTransportEvents* events = nullptr;
    QList<std::tuple<peer::Channel, QByteArray, bool>> sent;
    bool alive = true;
};

// The paired desktop as the runner knows it, and the key it signs its answers with.
struct Desktop {
    QString machineId;
    QString workspaceId;
    int keyVersion = 1;
    QByteArray signingPrivateKey;
    QString runnerId;
};
[[nodiscard]] Desktop desktopOf(FakeRelay& relay);

class Rig {
  public:
    // A state and work directory below `root`, for the Relay at `relay`.
    Rig(const QString& root, const QUrl& relay);
    ~Rig();
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    // `register <token>` through the command line, paired with the Relay's desktop at once.
    bool registerRunner(FakeRelay& relay);
    // The assembly and its loop; false when the assembly refused to start.
    bool start();
    // Ends the loop as a stop signal would and waits for it.
    bool stop(int timeoutMs);

    // The desktop's part of one job, once the runner leased it: signaling, welcome, the spec
    // with a real snapshot of `repo`, the pack. False when a step did not come in time.
    bool runDesktop(const Desktop& desktop, const QString& repo, protocol::JobSpec spec);
    // The runner's job.result, once sent.
    [[nodiscard]] std::optional<protocol::JobResult> result() const;
    [[nodiscard]] QByteArray output() const;
    void acknowledge(const QString& jobId);

    [[nodiscard]] QString stateDirectory() const { return m_state; }
    [[nodiscard]] QString workDirectory() const { return m_work; }
    [[nodiscard]] QStringList lines() const { return m_lines; }
    // The desktop step that did not come, after runDesktop() failed.
    [[nodiscard]] QString failedStep() const { return m_failedStep; }
    [[nodiscard]] bool exited() const { return m_exit.has_value(); }

    // A repository below the root holding `files`, committed.
    [[nodiscard]] QString repository(const QMap<QString, QByteArray>& files) const;

  private:
    void deliver(const protocol::Frame& frame);
    [[nodiscard]] QList<protocol::Frame> sentFrames() const;

    QString m_root;
    QString m_state;
    QString m_work;
    app::Environment m_environment;
    QStringList m_lines;
    int m_stops = 0;
    app::QtTimerHub m_timers;
    TestSocket* m_socket = nullptr;
    QList<std::shared_ptr<Peer>> m_peers;
    std::unique_ptr<peer::PeerTransportFactory> m_transports;
    struct Running;
    std::unique_ptr<Running> m_running;
    std::optional<int> m_exit;
    QString m_failedStep;
};

// Processes whose command line holds `marker`: their pid and command line, and their number.
[[nodiscard]] QStringList processesHolding(const QByteArray& marker);
[[nodiscard]] int processesWith(const QByteArray& marker);

} // namespace runner_assembly_test
