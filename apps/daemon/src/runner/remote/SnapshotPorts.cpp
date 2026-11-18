#include "runner/remote/SnapshotPorts.h"

#include <QRandomGenerator>

#include <algorithm>

namespace ketplus::daemon::runner::remote {

// One job: the sender's link, retention (by task id) and events.
class SnapshotPorts::Job final : public sync::SyncLink,
                                 public sync::SyncRetention,
                                 public sync::SyncEvents {
  public:
    Job(SnapshotPorts& owner, QString jobId, QString runnerId)
        : owner_(owner), jobId_(std::move(jobId)), runnerId_(std::move(runnerId)) {}

    // sync::SyncLink
    void send(const peer::protocol::Frame& frame) override {
        if (send_)
            send_(frame);
    }
    QString nextTransferId() override {
        quint64 value = 0;
        while (value == 0)
            value = QRandomGenerator::system()->generate64();
        return QStringLiteral("%1").arg(value, 16, 16, QLatin1Char('0'));
    }
    [[nodiscard]] bool bulkWritable() const override {
        return owner_.writable_ && owner_.writable_(jobId_);
    }
    void sendBulk(const QString& transferId, const QByteArray& bytes) override {
        if (sendBulk_)
            sendBulk_(transferId, bytes);
    }

    // sync::SyncRetention: confirmed snapshots by runner, packs and in-flight refs by task.
    [[nodiscard]] std::optional<QString> confirmed(const QString& runnerId,
                                                   const QString& workspaceKey) const override {
        return store_->retention->confirmed(runnerId, workspaceKey);
    }
    bool confirm(const QString& runnerId, const QString& workspaceKey,
                 const QString& snapshotId) override {
        return store_->retention->confirm(runnerId, workspaceKey, snapshotId);
    }
    void forget(const QString& runnerId, const QString& workspaceKey) override {
        store_->retention->forget(runnerId, workspaceKey);
    }
    [[nodiscard]] std::optional<sync::PackFile> pack(const QString&) const override {
        return store_->retention->pack(plan_.taskId);
    }
    void keepPack(const QString&, const QString& runnerId, const sync::PackFile& pack) override {
        store_->retention->keepPack(plan_.taskId, runnerId, pack);
    }
    void dropPack(const QString&) override { store_->retention->dropPack(plan_.taskId); }
    void release(const QString&) override { store_->retention->release(plan_.taskId); }

    // sync::SyncEvents
    void snapshotReady(const QString&) override { owner_.answer(jobId_, std::nullopt); }
    void snapshotFailed(const QString&, const QString& code) override {
        owner_.answer(jobId_, code);
    }

    SnapshotPorts& owner_;
    QString jobId_;
    QString runnerId_;
    SnapshotPlan plan_;
    SnapshotStores::Store* store_ = nullptr;
    Send send_;
    SendBulk sendBulk_;
    QMap<QString, QString> held_;
    quint64 generation_ = 0; // bumped by every start and cancel; a waiting start checks it
    bool attached_ = false;  // a connection is sending for the job
    std::unique_ptr<sync::JobSyncWork> work_;
    std::unique_ptr<sync::SnapshotSender> sender_;
};

SnapshotPorts::SnapshotPorts(SnapshotStores& stores, Plan plan, Accepted accepted,
                             BulkWritable writable, Answer answer, Defer defer)
    : stores_(stores), plan_(std::move(plan)), accepted_(std::move(accepted)),
      writable_(std::move(writable)), answer_(std::move(answer)), defer_(std::move(defer)) {}

SnapshotPorts::~SnapshotPorts() = default;

void SnapshotPorts::start(const QString& jobId, const QString& runnerId,
                          const QMap<QString, QString>& held, Send send, SendBulk sendBulk) {
    std::shared_ptr<Job>& job = jobs_[jobId];
    if (!job || job->runnerId_ != runnerId) {
        // The spec never reaches a second runner, so a different runner is a new job here.
        job = std::make_shared<Job>(*this, jobId, runnerId);
    }
    job->send_ = std::move(send);
    job->sendBulk_ = std::move(sendBulk);
    job->held_ = held;
    job->attached_ = true;
    begin(jobId, ++job->generation_, firstAcceptedRetryMs);
}

// Starts or resumes the job's sender once the record holds job.accepted.
void SnapshotPorts::begin(const QString& jobId, const quint64 generation, const qint64 retryMs) {
    const std::shared_ptr<Job> job = jobs_.value(jobId);
    if (!job || job->generation_ != generation || !job->attached_)
        return;
    if (!accepted_ || !accepted_(jobId, job->runnerId_)) {
        const std::weak_ptr<int> alive = alive_;
        defer_(retryMs, [this, alive, jobId, generation, retryMs] {
            if (!alive.expired())
                begin(jobId, generation, std::min(retryMs * 2, longestAcceptedRetryMs));
        });
        return;
    }
    if (job->sender_) {
        // A finished sender answers again: the runner of a resumed job already has it, or not.
        if (job->sender_->phase() == sync::SnapshotSender::Phase::Ready)
            answer(jobId, std::nullopt);
        else if (job->sender_->phase() == sync::SnapshotSender::Phase::Failed)
            answer(jobId, job->sender_->failure());
        else
            job->sender_->reconnected(job->held_);
        return;
    }
    const std::optional<SnapshotPlan> plan = plan_ ? plan_(jobId, job->runnerId_) : std::nullopt;
    SnapshotStores::Store* store = plan ? stores_.open(plan->storePath) : nullptr;
    if (!plan || !store || plan->job.jobId != jobId || plan->job.runnerId != job->runnerId_) {
        answer(jobId, QString(sync::code::snapshotBuildFailed));
        return;
    }
    job->plan_ = *plan;
    job->store_ = store;
    job->work_ = std::make_unique<sync::JobSyncWork>(*store->worker, plan->taskId);
    job->sender_ = std::make_unique<sync::SnapshotSender>(plan->job, *job, *job->work_, *job, *job);
    job->sender_->start(job->held_);
}

void SnapshotPorts::receive(const QString& jobId, const peer::protocol::Frame& frame) {
    const std::shared_ptr<Job> job = jobs_.value(jobId);
    if (job && job->attached_ && job->sender_)
        job->sender_->receive(frame);
}

void SnapshotPorts::bulkWritable(const QString& jobId) {
    const std::shared_ptr<Job> job = jobs_.value(jobId);
    if (job && job->attached_ && job->sender_)
        job->sender_->bulkWritable();
}

// The session stopped sending on its connection: a lost connection, a Stop or a result. The
// sender keeps its pack and its in-flight snapshot for a later connection; the record's end
// releases them.
void SnapshotPorts::cancel(const QString& jobId) {
    const std::shared_ptr<Job> job = jobs_.value(jobId);
    if (!job)
        return;
    ++job->generation_;
    job->attached_ = false;
    job->send_ = {};
    job->sendBulk_ = {};
    if (job->sender_)
        job->sender_->disconnected();
}

void SnapshotPorts::ended(const QString& jobId) {
    const std::shared_ptr<Job> job = jobs_.take(jobId);
    if (job && job->sender_)
        job->sender_->cancel(); // releases the task's pack and in-flight snapshot
}

bool SnapshotPorts::sending(const QString& jobId) const {
    const std::shared_ptr<Job> job = jobs_.value(jobId);
    return job && job->sender_ && !job->sender_->finished();
}

void SnapshotPorts::answer(const QString& jobId, const std::optional<QString>& code) {
    const std::weak_ptr<int> alive = alive_;
    defer_(0, [this, alive, jobId, code] {
        if (!alive.expired() && answer_)
            answer_(jobId, code);
    });
}

} // namespace ketplus::daemon::runner::remote
