#pragma once

#include "runner/peer/OutputIntake.h"
#include "runner/peer/PeerPorts.h"

#include <QDateTime>
#include <QMap>
#include <QString>

#include <optional>

// The desktop's side of one remote job on its peer connections: offer, `hello` and `welcome`,
// spec or resume, snapshot, output into the spool, result and cancel. The job record decides the
// job's state; the session only reports frames to it and follows its answers. Driven only by the
// calls below.
namespace ketplus::daemon::runner::peer {

inline constexpr qint64 helloTimeoutMs = 10'000;
inline constexpr qint64 firstRetryMs = 1'000;
inline constexpr qint64 maxRetryMs = 30'000;
inline constexpr qint64 spoolRetryMs = 5'000;

struct SessionPorts {
    PeerLinks& links;
    PeerClock& clock;
    JobRecords& records;
    JobSpecSource& specs;
    SnapshotPort& snapshots;
    OutputSpool& spool;
};

class JobPeerSession {
  public:
    enum class Phase {
        Waiting,   // no spec or resume sent yet
        Accepting, // `job.spec` or `job.resume` sent; waiting for the runner's answer
        Syncing,   // `job.accepted`: the snapshot goes out
        Running,   // `job.started`
        Finished,  // the result is durable, or the record ended the job
    };

    JobPeerSession(QString jobId, QString daemonVersion, SessionPorts ports);

    // A verified offer of `runnerId` for this job on a new connection. Ok lets signaling answer;
    // otherwise signaling ends with the code. Only the latest offer of a runner is kept.
    [[nodiscard]] RecordAnswer offered(const QString& connectionId, const QString& runnerId,
                                       qint64 attempt);
    // Signaling of an offered connection ended before its channels opened.
    void offerEnded(const QString& connectionId);
    // The three channels of an offered connection are open.
    void channelsOpened(const QString& connectionId);
    void frameReceived(const QString& connectionId, protocol::Channel channel,
                       const QByteArray& message);
    void bulkWritable(const QString& connectionId);
    // The connection is gone (not after the session closed it).
    void connectionLost(const QString& connectionId);
    void timerFired(PeerTimer timer);

    // Answers of the snapshot port; never from inside a call to it.
    void snapshotReady();
    void snapshotFailed(const QString& code);

    // The record asks the runner holding the job to cancel it (the user's Stop).
    void cancel(const QString& runnerId);
    // The record ended the job: every connection is closed, nothing more is sent.
    void close();

    [[nodiscard]] const QString& jobId() const { return jobId_; }
    [[nodiscard]] Phase phase() const { return phase_; }
    // The connection the session talks on, once welcomed.
    [[nodiscard]] QString openConnection() const;
    [[nodiscard]] bool owns(const QString& connectionId) const;
    [[nodiscard]] qint64 epoch() const { return epoch_; }

  private:
    enum class LinkState {
        Offered,       // signaling goes on; channels not open
        AwaitingHello, // channels open, `hello` due within 10 seconds
        Open,          // `welcome` sent
    };

    struct Link {
        QString connectionId;
        QString runnerId;
        qint64 attempt = 1;
        LinkState state = LinkState::Offered;
        qint64 epoch = 0;
        QMap<QString, QString> held; // `snapshots` of its `hello`
        QDateTime lastContact;
        bool cancelSent = false;
    };

    // Connections and the handshake (JobPeerSessionLink.cpp).
    [[nodiscard]] Link* linkOf(const QString& connectionId);
    void onHello(Link& link, const protocol::Hello& hello);
    void onBrokenFrame(Link& link, const protocol::FrameError& error);
    void closeLink(const QString& connectionId, const QString& reason, bool lost);
    void dropLink(const QString& connectionId);
    void linkLost(Link& link);
    bool send(const protocol::Frame& frame);
    void sendBulk(const QString& transferId, const QByteArray& bytes);

    // Spec, resume and the runner's answers (JobPeerSessionJob.cpp).
    void beginJob();
    [[nodiscard]] bool sendSpec(bool dispatch);
    void sendCancelIfStopping();
    void sendCancel(const QString& reason);
    void onFrame(Link& link, const protocol::Frame& frame);
    void onAccepted(const protocol::JobAccepted& accepted);
    void onRejected(const protocol::JobRejected& rejected);
    void onStarted(const protocol::JobStarted& started);
    void onResult(const protocol::JobResult& result);
    void recordResult();
    void startSnapshot();
    void stopSnapshot();
    void reportSnapshotFailure();
    void retryLater();

    // Output (JobPeerSessionOutput.cpp).
    void onOutput(const protocol::JobOutput& output);
    void onGap(const protocol::JobOutputGap& gap);
    void applyStep(const OutputStep& step);
    void sendAck(qint64 offset);
    void retrySpool();

    QString jobId_;
    QString daemonVersion_;
    SessionPorts ports_;
    Phase phase_ = Phase::Waiting;
    OutputIntake intake_;

    std::optional<Link> open_;    // the welcomed connection
    std::optional<Link> pending_; // an offered connection not yet welcomed
    qint64 epoch_ = 0;            // of the last `welcome`

    bool ackArmed_ = false;     // the output ack timer runs
    bool spoolBlocked_ = false; // the spool failed: output is dropped until the next resume
    bool spoolReported_ = false;
    bool snapshotRunning_ = false;
    QString snapshotRunner_;
    std::optional<QString> snapshotFailure_;    // a failure the record could not store yet
    std::optional<protocol::JobResult> result_; // a result the record could not store yet
    QString resultRunner_;
    qint64 retryMs_ = firstRetryMs;
};

} // namespace ketplus::daemon::runner::peer
