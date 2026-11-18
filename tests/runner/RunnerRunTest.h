#pragma once

#include "Environment.h"
#include "peer/PeerPorts.h"
#include "protocol/Frames.h"
#include "run/RunIdentity.h"
#include "run/RunPorts.h"
#include "run/RunnerLoop.h"
#include "session/SessionPorts.h"
#include "state/AcceptedJobsFile.h"
#include "state/JobJournalFile.h"
#include "state/PinTrust.h"
#include "state/RunnerState.h"
#include "state/StateDirectory.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QTemporaryDir>

#include <functional>
#include <memory>
#include <optional>

// The run loop over fakes for the Relay, its socket, the peer transport, the command runner and
// time; the job pieces, the journal, the accepted-jobs record and the pins are the real ones.
namespace runner_run_test {

using namespace ketplus::runner;
using namespace ketplus::runner::app;

// Vector 0 of the pairing tests.
namespace vectors {
inline constexpr const char* runnerSeed =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
inline constexpr const char* desktopSeed =
    "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f";
inline constexpr const char* desktopPublicKey =
    "29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd7";
inline constexpr const char* workspaceId = "ws_alpha";
inline constexpr const char* machineId = "m_desktop01";
inline constexpr const char* runnerId = "run_aaaaaaaaaaaa";
} // namespace vectors

// Values that must never reach a log line or the output.
inline constexpr const char* tokenMarker = "TOKEN-SECRET";
inline constexpr const char* commandMarker = "COMMAND-SECRET";

// Time for every timer of the run; nothing fires until advance().
class FakeHub final : public TimerHub {
  public:
    [[nodiscard]] QDateTime now() const override { return start.addMSecs(elapsed); }
    void arm(TimerKey key, qint64 delayMs) override;
    void disarm(TimerKey key) override;
    void onFired(Fired fired) override { m_fired = std::move(fired); }

    [[nodiscard]] bool armed(TimerKey key) const;
    [[nodiscard]] qint64 remaining(TimerKey key) const;
    // The next timer due, if any.
    [[nodiscard]] std::optional<std::pair<TimerKey, qint64>> next() const;
    // Fires the earliest due timer; false when none is armed.
    bool fireNext();
    // Fires `key` now, armed or not.
    void fire(TimerKey key);

    QDateTime start;
    qint64 elapsed = 0;
    QList<std::pair<TimerKey, qint64>> arms; // every arm, in order
    std::function<void(TimerKey, qint64)> onArm;

  private:
    [[nodiscard]] static int code(TimerKey key) {
        return static_cast<int>(key.domain) * 64 + key.id;
    }
    Fired m_fired;
    QMap<int, std::pair<TimerKey, qint64>> m_due; // code -> key, due at
};

// The Relay's HTTP routes. Each call spins a nested event loop like the real client and
// counts how deep calls go.
class FakeRelayPort final : public RelayPort {
  public:
    [[nodiscard]] AccessTokens::Call heartbeat(const QJsonObject& body) override;
    [[nodiscard]] delivery::CallResult lease(const delivery::LeaseCall& call) override;
    [[nodiscard]] Token token(bool renew) override;
    [[nodiscard]] std::optional<ExitCode> pairings() override;

    // "heartbeat", "pairings", "token", "token renew", "acquire", "renew", "start", "finish",
    // "release".
    QStringList calls;
    QList<QJsonObject> heartbeats;
    QList<delivery::LeaseCall> leases;
    int depth = 0;
    int maxDepth = 0;
    int spinMs = 1;
    bool pending = false; // the heartbeat's `pending`
    std::function<AccessTokens::Call(const QJsonObject&)> heartbeatAnswer;
    std::function<delivery::CallResult(const delivery::LeaseCall&)> leaseAnswer;
    std::function<Token(bool)> tokenAnswer;
    std::optional<ExitCode> pairingsExit;
    // Called inside each call's nested loop, with its name.
    std::function<void(const QString&)> during;

  private:
    void enter(const QString& name);
    void leave() { --depth; }
};

class FakeSocket final : public peer::RelaySocket {
  public:
    void open(const QString& accessToken) override;
    bool send(const QJsonObject& message) override;
    void close() override { live = false; }

    QStringList tokens;
    QList<QJsonObject> sent;
    bool live = false;
    std::function<void(const QJsonObject&)> onSend; // after each message sent
};

struct TransportState {
    peer::PeerTransportEvents* events = nullptr;
    bool offered = false;
    QStringList answers;
    QList<std::tuple<peer::Channel, QByteArray, bool>> sent;
    bool closed = false;
    bool alive = true;
};

class FakeTransports final : public peer::PeerTransportFactory {
  public:
    [[nodiscard]] std::unique_ptr<peer::PeerTransport>
    create(peer::PeerTransportEvents& events) override;
    QList<std::shared_ptr<TransportState>> made;
};

class FakeCommand final : public CommandHost, public session::CommandRunner {
  public:
    [[nodiscard]] session::CommandRunner& runner() override { return *this; }
    void bind(CommandEvents bound) override { events = std::move(bound); }
    [[nodiscard]] Readiness readiness() const override { return state; }
    void onReadinessChanged(Changed changed) override { m_changed = std::move(changed); }
    void cleanUpLeftovers(std::function<void(bool)> done) override;
    [[nodiscard]] bool admit() override { return state == Readiness::Ready; }
    void launch(const session::LaunchRequest& request) override;
    void kill() override { ++kills; }

    void set(Readiness next);
    void finishCleanup(bool clean);

    Readiness state = Readiness::Ready;
    std::optional<bool> autoCleanup = true; // answers the clean-up at once when set
    CommandEvents events;
    QList<session::LaunchRequest> launches;
    int kills = 0;
    QStringList* order = nullptr; // "cleanup" when the clean-up is asked

  private:
    Changed m_changed;
    std::function<void(bool)> m_cleanupDone;
};

class FakeReceiver final : public session::SnapshotReceiver {
  public:
    [[nodiscard]] QMap<QString, QString> heldSnapshots(const QString&) const override { return {}; }
    void begin(const protocol::JobSpec& spec, const QString& machineId, Send send) override;
    void receive(const protocol::Frame& frame) override { frames.append(frame); }
    void receivePayload(const QString& transferId, const QByteArray& payload) override {
        payloads.insert(transferId, payload);
    }
    void cancel() override { ++cancels; }

    QList<protocol::JobSpec> begun;
    QList<protocol::Frame> frames;
    QMap<QString, QByteArray> payloads;
    int cancels = 0;
    int idles = 0;
    std::function<void(const QString&, const QStringList&)> ready;
    std::function<void(const QString&)> failed;
};

// One run over a state directory that holds the active pin of the vector desktop.
struct Run {
    Run();
    ~Run();

    // A loop over the current parts; `receiver` replaces the fake one.
    void make(const ReceiverHost* receiver = nullptr);
    void start();
    // Runs queued Relay calls until the queue is idle.
    void settle();
    // Moves time on by `ms`, firing every timer due on the way.
    void advance(qint64 ms);
    // The Relay socket opens and says ready.
    void socketReady();
    void wakeup(const QJsonObject& message);
    // A heartbeat answered with pending:true, then the acquire answered with a lease of `jobId`.
    void acquire(const QString& jobId = QStringLiteral("job_42"));
    // signal.start → started → offer → signed answer → opened; then welcome.
    void connectDesktop();
    void deliver(const protocol::Frame& frame);
    // Frames the runner sent on the open connection.
    [[nodiscard]] QList<protocol::Frame> sentFrames() const;
    [[nodiscard]] QStringList sentTypes() const;
    [[nodiscard]] QStringList leaseKinds() const;
    [[nodiscard]] QString log() const { return lines.join(QLatin1Char('\n')); }
    [[nodiscard]] std::optional<delivery::JournalRecord> journalRecord() const;
    [[nodiscard]] QString heartbeatState(qsizetype index = -1) const;

    QTemporaryDir root;
    std::optional<StateDirectory> state;
    std::unique_ptr<RunnerState> runnerState;
    RunnerIdentity identity;
    Environment environment;
    QStringList lines; // out and err
    int stops = 0;
    FakeHub hub;
    FakeRelayPort relay;
    FakeSocket* socket = nullptr;
    FakeTransports transports;
    peer::PeerTransportFactory* realTransports = nullptr; // replaces `transports` when set
    FakeCommand command;
    FakeReceiver receiver;
    std::unique_ptr<IdentitySigner> signer;
    std::unique_ptr<StatePins> pins;
    std::unique_ptr<PinTrust> trust;
    std::unique_ptr<JobJournalFile> journal;
    std::optional<AcceptedJobsFile> accepted;
    QString acceptedError;
    std::unique_ptr<RunnerLoop> loop;
    std::optional<ExitCode> exit;
    QString connectionId = QStringLiteral("pc_abcdefghijklmnopqrstuvwx");
    int acquireCount = 0;
};

[[nodiscard]] QJsonObject
leaseReply(const QString& jobId, const QString& leaseId,
           const QString& machineId = QString::fromLatin1(vectors::machineId));
// signal.answer for `sdp`, signed by the vector desktop.
[[nodiscard]] QJsonObject signedAnswerFor(const QString& connectionId, const QString& jobId,
                                          const QString& offer, const QString& sdp,
                                          const QDateTime& issuedAt);
[[nodiscard]] delivery::CallResult ok(const QJsonObject& body = {});
[[nodiscard]] delivery::CallResult refused(const QString& code, const QJsonObject& body = {});
[[nodiscard]] protocol::JobSpec makeSpec(const QString& jobId = QStringLiteral("job_42"));
[[nodiscard]] inline int count(const QStringList& list, const QString& value) {
    return static_cast<int>(list.count(value));
}

class RunnerRunStartTest : public QObject {
    Q_OBJECT
  private slots:
    void cleanupComesBeforeTheJournalAndAnyAcquire();
    void cleanupThatNeverAnswersIsBounded();
    void heartbeatReportsDrainingUntilStarted();
    void readyRunnerReportsIdleEveryThirtySeconds();
    void acquireOnlyWhenEveryConditionHolds();
    void oneAcquirePerHeartbeatAnswer();
    void jobAvailableAcquiresThatJob();
    void executorReadinessDrainsAndComesBack();
    void readinessLostBeforeTheSpecReleasesTheLease();
    void stateChangesSendAHeartbeatSoon();
    void brokenJournalDrainsUntilRestart();
    void brokenAcceptedJobsDrainUntilRestart();
    void restoredLeaseWithoutSpecIsReleasedWhileDraining();
    void restoredStartedJobFinishesInterrupted();
    void relayCallsNeverNest();
    void heartbeatsWaitAtMostOnce();
    void cancelWakeupRenewsOnce();
    void logsHoldNoSecret();
};

class RunnerRunJobTest : public QObject {
    Q_OBJECT
  private slots:
    void wholeJobThenTheNextOne();
    void wholeJobWithARealSnapshot();
    void jobOverARealPeerConnection();
    void unreachableDesktopReleasesPeerUnreachable();
    void unpinnedDesktopReleasesUntrustedRequester();
    void sessionRefusesTheSpec();
    void reconnectAfterALostConnection();
    void failedRenewalStopsTheJob();
    void lostRelaySocketKeepsTheJob();
    void desktopCancel();
    void jobTimeout();
};

class RunnerRunStopTest : public QObject {
    Q_OBJECT
  private slots:
    void stopWhileIdleExitsZero();
    void stopWaitsForAnAcquireInFlight();
    void stopWithAJobWaitsForItsFinish();
    void secondSignalExitsAtOnce();
    void restartAfterASecondSignal();
    void fatalRefusalsExitAtOnce_data();
    void fatalRefusalsExitAtOnce();
    void exitInsideANestedCallDestroysNothing();
};

} // namespace runner_run_test
