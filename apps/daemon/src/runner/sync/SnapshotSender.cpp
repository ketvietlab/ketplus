#include "runner/sync/SnapshotSender.h"

#include <QLoggingCategory>

#include <utility>

namespace ketplus::daemon::runner::sync {

namespace {

Q_LOGGING_CATEGORY(lcSync, "ketplus.runner.sync")

constexpr qint64 chunkBytes = protocol::limits::chunkBytes;

} // namespace

SnapshotSender::SnapshotSender(SyncJob job, SyncLink& link, SyncWork& work,
                               SyncRetention& retention, SyncEvents& events, SenderLimits limits)
    : job_(std::move(job)), link_(link), work_(work), retention_(retention), events_(events),
      limits_(limits) {}

SnapshotSender::~SnapshotSender() = default;

bool SnapshotSender::finished() const {
    return phase_ == Phase::Ready || phase_ == Phase::Failed || phase_ == Phase::Cancelled;
}

void SnapshotSender::start(const QMap<QString, QString>& snapshots) {
    if (phase_ != Phase::Idle)
        return;
    snapshots_ = snapshots;
    build(baseFor(snapshots_));
}

QString SnapshotSender::baseFor(const QMap<QString, QString>& snapshots) const {
    const auto confirmed = retention_.confirmed(job_.runnerId, job_.workspaceKey);
    if (!confirmed || confirmed->isEmpty())
        return {};
    return snapshots.value(job_.workspaceKey) == *confirmed ? *confirmed : QString{};
}

void SnapshotSender::build(const QString& baseId) {
    stopTransfer();
    pack_.reset();
    phase_ = Phase::Packing;
    const quint64 generation = ++generation_;
    const std::weak_ptr<int> alive = alive_;
    work_.buildPack(job_.snapshotId, baseId,
                    [this, alive, generation, baseId](const PackOutcome& outcome) {
                        if (alive.expired() || generation != generation_)
                            return;
                        packBuilt(outcome, baseId);
                    });
}

void SnapshotSender::packBuilt(const PackOutcome& outcome, const QString& baseId) {
    if (phase_ != Phase::Packing)
        return;
    if (!outcome.pack) {
        if (outcome.baseMissing && !baseId.isEmpty()) {
            retention_.forget(job_.runnerId, job_.workspaceKey);
            build({});
            return;
        }
        fail(outcome.code.isEmpty() ? code::snapshotBuildFailed : outcome.code);
        return;
    }
    const PackFile& pack = *outcome.pack;
    if (pack.snapshotId != job_.snapshotId || pack.baseId != baseId || pack.size < 1) {
        fail(code::snapshotBuildFailed);
        return;
    }
    if (pack.size > limits_.maxPackBytes) {
        fail(code::snapshotTooLarge);
        return;
    }
    // The hello may have changed while the pack was built; a base the runner no longer lists
    // cannot be used.
    if (!baseId.isEmpty() && snapshots_.value(job_.workspaceKey) != baseId) {
        build(baseFor(snapshots_));
        return;
    }
    pack_ = pack;
    retention_.keepPack(job_.jobId, pack);
    offer();
}

void SnapshotSender::offer() {
    stopTransfer();
    acked_ = 0;
    next_ = 0;
    protocol::SnapshotOffer frame;
    frame.jobId = job_.jobId;
    frame.snapshotId = job_.snapshotId;
    if (!pack_->baseId.isEmpty())
        frame.baseId = pack_->baseId;
    frame.packBytes = pack_->size;
    frame.packSha256 = pack_->sha256;
    frame.chunkBytes = chunkBytes;
    frame.warnings = job_.warnings;
    phase_ = Phase::Offered;
    qCInfo(lcSync).noquote() << "snapshot offer" << job_.jobId << pack_->size << "bytes"
                             << (pack_->baseId.isEmpty() ? "full" : "incremental");
    link_.send(frame);
}

bool SnapshotSender::matches(const QString& snapshotId) const {
    return snapshotId == job_.snapshotId;
}

bool SnapshotSender::onBoundary(const qint64 offset) const {
    return offset == pack_->size || offset % chunkBytes == 0;
}

void SnapshotSender::receive(const protocol::Frame& frame) {
    if (const auto* value = std::get_if<protocol::SnapshotWant>(&frame))
        want(*value);
    else if (const auto* value = std::get_if<protocol::SnapshotAck>(&frame))
        ack(*value);
    else if (const auto* value = std::get_if<protocol::SnapshotError>(&frame))
        error(*value);
    else if (const auto* value = std::get_if<protocol::SnapshotReady>(&frame);
             value && matches(value->snapshotId))
        ready();
}

void SnapshotSender::want(const protocol::SnapshotWant& frame) {
    if (phase_ != Phase::Offered || !matches(frame.snapshotId))
        return;
    if (frame.state == QStringLiteral("have")) {
        phase_ = Phase::AwaitReady;
        return;
    }
    if (frame.state != QStringLiteral("need"))
        return;
    if (frame.fromOffset < 0 || frame.fromOffset > pack_->size || !onBoundary(frame.fromOffset)) {
        fail(code::packInvalid);
        return;
    }
    acked_ = frame.fromOffset;
    next_ = frame.fromOffset;
    phase_ = Phase::Sending;
    pump();
}

void SnapshotSender::ack(const protocol::SnapshotAck& frame) {
    if (phase_ != Phase::Sending || !matches(frame.snapshotId))
        return;
    if (frame.offset == acked_)
        return;
    if (frame.offset < acked_ || frame.offset > next_ || !onBoundary(frame.offset)) {
        fail(code::packInvalid);
        return;
    }
    acked_ = frame.offset;
    pump();
}

void SnapshotSender::error(const protocol::SnapshotError& frame) {
    const bool active =
        phase_ == Phase::Offered || phase_ == Phase::Sending || phase_ == Phase::AwaitReady;
    if (!active || !matches(frame.snapshotId))
        return;
    if (frame.code == QStringLiteral("chunk_corrupt")) {
        if (phase_ != Phase::Sending)
            return;
        stopTransfer();
        next_ = acked_;
        pump();
        return;
    }
    if (frame.code == QStringLiteral("base_missing")) {
        if (pack_->baseId.isEmpty()) {
            fail(code::packInvalid);
            return;
        }
        retention_.forget(job_.runnerId, job_.workspaceKey);
        retention_.dropPack(job_.jobId);
        build({});
        return;
    }
    fail(frame.code);
}

void SnapshotSender::ready() {
    if (phase_ != Phase::Sending && phase_ != Phase::AwaitReady)
        return;
    stopTransfer();
    if (!retention_.confirm(job_.runnerId, job_.workspaceKey, job_.snapshotId)) {
        fail(code::storageError);
        return;
    }
    phase_ = Phase::Ready;
    ++generation_;
    retention_.release(job_.jobId);
    qCInfo(lcSync).noquote() << "snapshot ready" << job_.jobId;
    events_.snapshotReady(job_.jobId);
}

void SnapshotSender::pump() {
    if (phase_ != Phase::Sending)
        return;
    if (pending_) {
        if (!link_.bulkWritable())
            return;
        const PendingChunk chunk = std::move(*pending_);
        pending_.reset();
        sendChunk(chunk.bytes, chunk.sha256, chunk.offset);
    }
    while (phase_ == Phase::Sending && !reading_ && !pending_ && next_ < pack_->size &&
           next_ - acked_ < limits_.maxChunksInFlight * chunkBytes && link_.bulkWritable()) {
        const qint64 offset = next_;
        const qint64 length = qMin(chunkBytes, pack_->size - offset);
        reading_ = true;
        const quint64 generation = generation_;
        const std::weak_ptr<int> alive = alive_;
        work_.readChunk(*pack_, offset, length,
                        [this, alive, generation, offset, length](const ChunkRead& chunk) {
                            if (alive.expired() || generation != generation_)
                                return;
                            chunkRead(chunk, offset, length);
                        });
    }
}

void SnapshotSender::chunkRead(const ChunkRead& chunk, const qint64 offset, const qint64 length) {
    reading_ = false;
    if (phase_ != Phase::Sending)
        return;
    if (chunk.bytes.size() != length || chunk.sha256.size() != 64) {
        fail(code::packUnreadable);
        return;
    }
    if (!link_.bulkWritable()) {
        pending_ = PendingChunk{chunk.bytes, chunk.sha256, offset};
        return;
    }
    sendChunk(chunk.bytes, chunk.sha256, offset);
    pump();
}

void SnapshotSender::sendChunk(const QByteArray& bytes, const QString& sha256,
                               const qint64 offset) {
    protocol::SnapshotChunk frame;
    frame.snapshotId = job_.snapshotId;
    frame.offset = offset;
    frame.bytes = bytes.size();
    frame.sha256 = sha256;
    frame.transferId = link_.nextTransferId();
    link_.send(frame);
    link_.sendBulk(frame.transferId, bytes);
    next_ = offset + bytes.size();
}

void SnapshotSender::bulkWritable() { pump(); }

void SnapshotSender::stopTransfer() {
    ++generation_;
    reading_ = false;
    pending_.reset();
}

void SnapshotSender::disconnected() {
    if (finished() || phase_ == Phase::Idle || phase_ == Phase::Packing)
        return;
    stopTransfer();
    phase_ = Phase::Detached;
}

void SnapshotSender::reconnected(const QMap<QString, QString>& snapshots) {
    if (finished() || phase_ == Phase::Idle)
        return;
    snapshots_ = snapshots;
    if (phase_ == Phase::Packing)
        return; // the base is checked again when the pack arrives
    stopTransfer();
    const auto kept = retention_.pack(job_.jobId);
    const bool baseHeld =
        kept && (kept->baseId.isEmpty() || snapshots_.value(job_.workspaceKey) == kept->baseId);
    if (kept && baseHeld && kept->snapshotId == job_.snapshotId) {
        pack_ = kept;
        offer();
        return;
    }
    retention_.dropPack(job_.jobId);
    build(baseFor(snapshots_));
}

void SnapshotSender::cancel() {
    if (finished())
        return;
    stopTransfer();
    phase_ = Phase::Cancelled;
    retention_.release(job_.jobId);
}

void SnapshotSender::fail(const QString& failure) {
    stopTransfer();
    phase_ = Phase::Failed;
    failure_ = failure;
    retention_.release(job_.jobId);
    qCWarning(lcSync).noquote() << "snapshot failed" << job_.jobId << failure;
    events_.snapshotFailed(job_.jobId, failure);
}

} // namespace ketplus::daemon::runner::sync
