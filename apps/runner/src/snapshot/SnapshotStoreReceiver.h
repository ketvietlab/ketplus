#pragma once

#include "session/SessionPorts.h"
#include "snapshot/ReceiverStore.h"
#include "snapshot/ReceiverWork.h"

#include <QMap>
#include <QMetaObject>
#include <QObject>
#include <QSet>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

// The runner's SnapshotReceiver: takes a job's pack chunk by chunk into a partial pack, then
// into the repository's mirror and the workspace, through the snapshot library. Disk and Git
// work runs on one worker thread; the session hears back only on the thread that made this.
namespace ketplus::runner::receiver {

struct ReceiverOptions {
    qint64 budgetBytes = qint64(50) * 1024 * 1024 * 1024; // workspaces, mirrors, partial packs
    qint64 partialKeepMs = qint64(24) * 60 * 60 * 1000;   // since the last chunk written
    std::function<QDateTime()> now = [] { return QDateTime::currentDateTimeUtc(); };
    ChunkWriter writer = writeAndSync;
    snapshot::GitTool git;
    std::function<void(WorkStep)> hold; // tests: called on the worker before each step
};

class SnapshotStoreReceiver final : public session::SnapshotReceiver {
  public:
    using Ready = std::function<void(const QString& workspace, const QStringList& warnings)>;
    using Failed = std::function<void(const QString& code)>;

    // Refuses a work directory inside the state directory or the other way round.
    [[nodiscard]] static std::unique_ptr<SnapshotStoreReceiver>
    create(const app::StateDirectory& state, const QString& workRoot, ReceiverOptions options,
           QString* error);
    ~SnapshotStoreReceiver() override;

    // Where the answers for the session go (JobSession::snapshotReady and snapshotFailed).
    void answerTo(Ready ready, Failed failed);

    [[nodiscard]] QMap<QString, QString> heldSnapshots(const QString& machineId) const override;
    void begin(const protocol::JobSpec& spec, const QString& machineId, Send send) override;
    void receive(const protocol::Frame& frame) override;
    void receivePayload(const QString& transferId, const QByteArray& payload) override;
    void cancel() override;

    // The runner holds no job: drop stale partial packs, measure, and evict the least
    // recently used workspaces while over budget. Until then the last job's workspace stays.
    void idle();

    // Bytes the stored sizes add up to, as the offer path sees them.
    [[nodiscard]] qint64 usedBytes() const;

  private:
    using Key = std::pair<QString, QString>; // repositoryKey, workspaceKey
    enum class Phase { WaitingOffer, Probing, Receiving, Applying };
    struct Job {
        protocol::JobSpec spec;
        QString machineId;
        Send send;
        Phase phase = Phase::WaitingOffer;
        std::optional<protocol::SnapshotOffer> offer;
        PartialRecord partial;
        qint64 next = 0;
        QMap<QString, protocol::SnapshotChunk> frames; // by transferId, payload not yet here
        QMap<QString, QByteArray> payloads;            // by transferId, frame not yet here
        QMap<qint64, std::pair<protocol::SnapshotChunk, QByteArray>> waiting; // by offset
        bool writing = false;
        int corruptInARow = 0;
        bool baseMissingAfterPack = false;
        std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    };

    SnapshotStoreReceiver(const app::StateDirectory& state, const QString& workRoot,
                          ReceiverOptions options);

    // Offers (ReceiverOffer.cpp).
    void onOffer(const protocol::SnapshotOffer& offer);
    void probed(quint64 generation, const ProbeResult& result);
    [[nodiscard]] std::optional<QList<Eviction>> evictionsFor(qint64 needed,
                                                              const std::optional<Key>& keep) const;
    void receivePack(quint64 generation, const std::optional<PartialRecord>& resumed);
    void startApply(bool fromMirror);
    void applied(quint64 generation, const ApplyOutcome& outcome, bool fromMirror);
    void adopt(const StoredRecords& records);

    // Chunks (ReceiverChunks.cpp).
    void onChunk(const protocol::SnapshotChunk& chunk);
    void pair(const QString& transferId);
    void pump();
    void written(quint64 generation, const ChunkResult& result);
    void corrupt();
    void clearHeld();
    void dropPartial(); // the current workspace's partial pack and its record

    void sendFrame(const protocol::Frame& frame);
    void fail(const QString& code);
    void forget(const Key& key);
    [[nodiscard]] Key key() const;
    // Runs `job` on the worker, then `done` here unless the job was replaced meanwhile.
    template <typename T>
    void work(std::function<T()> job, std::function<void(quint64, const T&)> done);

    ReceiverStore m_store;
    ReceiverOptions m_options;
    WorkContext m_context;
    QObject m_home; // answers from the worker land on this thread
    Ready m_ready;
    Failed m_failed;
    QMap<QString, RepositoryRecord> m_repositories;
    QMap<Key, WorkspaceRecord> m_workspaces;
    QMap<Key, PartialRecord> m_partials;
    QSet<QString> m_orphans;
    std::optional<Key> m_inUse; // the workspace of the last job, until idle()
    std::optional<Job> m_job;
    quint64 m_generation = 0;
    ReceiverWorker m_worker; // last: stops before anything its jobs use goes away
};

template <typename T>
void SnapshotStoreReceiver::work(std::function<T()> job,
                                 std::function<void(quint64, const T&)> done) {
    QObject* home = &m_home;
    m_worker.post([home, generation = m_generation, job = std::move(job), done = std::move(done)] {
        T result = job();
        QMetaObject::invokeMethod(
            home, [generation, done, result = std::move(result)] { done(generation, result); },
            Qt::QueuedConnection);
    });
}

} // namespace ketplus::runner::receiver
