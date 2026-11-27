#pragma once

#include "runner/jobs/LedgerRecordStore.h"
#include "runner/jobs/RemoteJobCoordinator.h"
#include "runner/jobs/RemoteJobSpecs.h"
#include "runner/link/RunnerLinks.h"
#include "runner/peer/RemotePeerSessions.h"
#include "runner/remote/CoordinatorRecords.h"
#include "runner/remote/RelayCancels.h"
#include "runner/remote/SnapshotPorts.h"
#include "runner/remote/TaskSnapshots.h"
#include "runner/sync/SnapshotPreparation.h"

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QSqlDatabase>
#include <QString>

#include <functional>
#include <memory>

namespace ketplus::daemon {
class BackgroundTaskLedger;
}

// The desktop's remote jobs in the running daemon: the records in the background task registry,
// the specs held in memory, the coordinator, one peer session per job with its output spool and
// snapshot sender, and the runner links on the Relay socket. Every input runs on the event loop;
// Git and file work run where the parts already put them.
namespace ketplus::daemon::runner::remote {

// A request of the layer above for a task created with runs_on and approved.
struct SubmitRequest {
    peer::protocol::JobSpec spec; // `cwd` absolute; snapshot fields are filled here
    QStringList runsOn; // any order; the task was created with jobs::normalizedRunsOn of it
    qint64 timeoutMs = 0;
};

struct RemoteJobsPorts {
    QSqlDatabase database;
    BackgroundTaskLedger* ledger = nullptr;
    jobs::JobRelay* relay = nullptr;
    jobs::JobClock* jobClock = nullptr;
    const jobs::RunnerTrust* trust = nullptr;
    peer::PeerClock* peerClock = nullptr;
    SnapshotStores* stores = nullptr;
    QString daemonVersion;
    // Whether the daemon is signed in and the Relay offers runners to it.
    std::function<bool()> relayAvailable;
    // Changes whenever the account or the enrollment does.
    std::function<QString()> accountScope;
    // The usable system git: empty, or a sync::prepare code.
    QString gitProblem;
    // The desktop runner secret.
    TaskSnapshots::Secret secret;
    // Freezes a worktree off the event loop and answers on it.
    std::function<void(sync::PrepareRequest, std::function<void(sync::PreparedOutcome)>)> prepare;
    // The runner links for the current account, answering into `events`; null when this
    // machine has no identity.
    std::function<std::unique_ptr<link::RunnerLinks>(link::LinkEvents& events)> makeLinks;
    // The output spool of a task in the registry's output store; null when it cannot be opened.
    std::function<std::unique_ptr<peer::OutputSpool>(const QString& taskId)> openSpool;
    // Runs `work` on the event loop after `delayMs`, never inside the caller.
    SnapshotPorts::Defer defer;
};

class RemoteJobs final : public link::LinkEvents {
  public:
    using Callback = std::function<void(const QString& code)>;
    using Changed = std::function<void(const jobs::RemoteJobRecord& record)>;

    explicit RemoteJobs(RemoteJobsPorts ports);
    ~RemoteJobs() override;
    RemoteJobs(const RemoteJobs&) = delete;
    RemoteJobs& operator=(const RemoteJobs&) = delete;

    // After a daemon start, once: loads the records and ends those the restart cut off.
    void restore();
    void setChanged(Changed changed) { changed_ = std::move(changed); }

    // Signed in, runners offered, Git usable and the registry open.
    [[nodiscard]] bool available() const;
    // Why a remote job cannot start now: empty, runner_unavailable or git_unsupported.
    [[nodiscard]] QString refusal() const;
    // Records not ended yet; tasks still waiting for their first record are not among them.
    [[nodiscard]] int outstanding() const { return coordinator_.outstanding(); }
    void submit(const QString& taskId, SubmitRequest request, Callback callback);
    [[nodiscard]] jobs::JobDecision stop(const QString& taskId);
    // A runner the user removed or unpaired; `clientId` of the request, or "daemon".
    [[nodiscard]] jobs::JobDecision runnerRemoved(const QString& runnerId, const QString& clientId);
    [[nodiscard]] std::optional<jobs::RemoteJobRecord> record(const QString& taskId) const;

    // The Relay socket: true when the message was this service's.
    bool relayMessage(const QJsonObject& message);
    // The socket opened again after it was closed.
    void relaySocketReopened();
    // Timers of the parts.
    void jobTimer(const QString& taskId, jobs::JobTimer timer);
    void peerTimer(const QString& jobId, peer::PeerTimer timer);
    void linkTimer(const QString& connectionId);
    // Hourly: pack cache and store objects.
    void sweep(const QDateTime& now);

    [[nodiscard]] jobs::RemoteJobCoordinator& coordinator() { return coordinator_; }
    [[nodiscard]] peer::RemotePeerSessions& sessions() { return sessions_; }
    [[nodiscard]] jobs::LedgerRecordStore& store() { return store_; }
    [[nodiscard]] bool linksOpen() const { return links_ != nullptr; }

    // link::LinkEvents
    [[nodiscard]] peer::RecordAnswer offered(const QString& connectionId, const QString& runnerId,
                                             const QString& jobId, qint64 attempt) override;
    void offerEnded(const QString& connectionId) override;
    void channelsOpened(const QString& connectionId) override;
    void frameReceived(const QString& connectionId, link::FrameChannel channel,
                       const QByteArray& message) override;
    void bulkWritable(const QString& connectionId) override;
    void connectionLost(const QString& connectionId) override;

  private:
    // The sessions' links: those of the current account, which change with it.
    class CurrentLinks final : public peer::PeerLinks {
      public:
        explicit CurrentLinks(const std::unique_ptr<link::RunnerLinks>& links) : links_(links) {}
        void send(const QString& connectionId, peer::protocol::Channel channel,
                  const QByteArray& message) override;
        void sendBulk(const QString& connectionId, const QString& transferId,
                      const QByteArray& bytes) override;
        [[nodiscard]] bool bulkWritable(const QString& connectionId) const override;
        void close(const QString& connectionId, const QString& reason) override;

      private:
        const std::unique_ptr<link::RunnerLinks>& links_;
    };

    void prepared(const QString& taskId, const SubmitRequest& request,
                  const sync::PreparedOutcome& outcome, const Callback& callback);
    void recorded(const jobs::RemoteJobRecord& record);
    void ended(const jobs::RemoteJobRecord& record);
    // Ends the records a restart cut off before the runner took the spec.
    void endUnsent(const QList<jobs::RemoteJobRecord>& loaded);
    void endUnreadable(const QStringList& taskIds);
    void ensureLinks();
    void dropLinks();
    [[nodiscard]] std::unique_ptr<peer::JobPeerSession> makeSession(const QString& jobId);
    [[nodiscard]] std::optional<TaskSnapshots::JobOwner> ownerOf(const QString& jobId) const;
    [[nodiscard]] bool acceptedBy(const QString& jobId, const QString& runnerId);

    RemoteJobsPorts ports_;
    bool open_ = false; // the registry could be initialized
    jobs::LedgerRecordStore store_;
    jobs::RemoteJobSpecs specs_;
    peer::RemotePeerSessions sessions_;
    jobs::RemoteJobCoordinator coordinator_;
    CoordinatorRecords records_;
    TaskSnapshots snapshots_;
    SnapshotPorts snapshotPorts_;
    RelayCancels cancels_;
    std::unique_ptr<link::RunnerLinks> links_;
    CurrentLinks currentLinks_{links_};
    QString linksScope_;
    QHash<QString, std::shared_ptr<peer::OutputSpool>> spools_; // by job id
    QHash<QString, bool> submitting_;                           // tasks being prepared
    QSet<QString> live_;                                        // tasks whose record goes on
    // Taken offers by connection id: whether channels opened. May keep ids the links ended
    // without a word (a close or a newer connection); dropLinks() asks the links first.
    QHash<QString, bool> connections_;
    Changed changed_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace ketplus::daemon::runner::remote
