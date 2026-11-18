#include "protocol/FrameLimits.h"
#include "snapshot/SnapshotStoreReceiver.h"

namespace ketplus::runner::receiver {
namespace {

constexpr qsizetype maxHeldChunks = 4; // paired or not, beyond the one being written
constexpr int maxCorruptInARow = 8;

} // namespace

void SnapshotStoreReceiver::onChunk(const protocol::SnapshotChunk& chunk) {
    if (!m_job || !m_job->offer || m_job->phase == Phase::Applying)
        return; // no offer yet, or every byte is here
    m_job->frames.insert(chunk.transferId, chunk);
    pair(chunk.transferId);
}

void SnapshotStoreReceiver::receivePayload(const QString& transferId, const QByteArray& payload) {
    if (!m_job || !m_job->offer || m_job->phase == Phase::Applying)
        return;
    m_job->payloads.insert(transferId, payload);
    pair(transferId);
}

void SnapshotStoreReceiver::pair(const QString& transferId) {
    Job& job = *m_job;
    if (job.frames.contains(transferId) && job.payloads.contains(transferId)) {
        const protocol::SnapshotChunk chunk = job.frames.take(transferId);
        const QByteArray payload = job.payloads.take(transferId);
        if (chunk.snapshotId != job.offer->snapshotId || chunk.bytes <= 0 ||
            chunk.bytes > protocol::limits::chunkBytes ||
            chunk.offset + chunk.bytes > job.offer->packBytes) {
            corrupt();
            return;
        }
        if (job.phase == Phase::Receiving && chunk.offset < job.next) {
            sendFrame(protocol::SnapshotAck{job.offer->snapshotId, job.next}); // a repeat
            return;
        }
        job.waiting.insert(chunk.offset, {chunk, payload});
        pump();
    }
    if (job.frames.size() + job.payloads.size() + job.waiting.size() > maxHeldChunks)
        corrupt();
}

void SnapshotStoreReceiver::pump() {
    Job& job = *m_job;
    if (job.phase != Phase::Receiving || job.writing)
        return;
    const auto it = job.waiting.constFind(job.next);
    if (it == job.waiting.cend())
        return;
    const protocol::SnapshotChunk chunk = it->first;
    const QByteArray payload = it->second;
    job.waiting.erase(it);
    job.writing = true;
    const PartialRecord partial = job.partial;
    const Key current = key();
    work<ChunkResult>(
        [this, partial, payload, chunk] {
            return writeChunk(m_context, partial, payload, chunk.bytes, chunk.sha256);
        },
        [this, current](const quint64 generation, const ChunkResult& result) {
            if (result.status == WriteStatus::Ok)
                m_partials.insert(current, result.partial);
            written(generation, result);
        });
}

void SnapshotStoreReceiver::written(const quint64 generation, const ChunkResult& result) {
    if (generation != m_generation || !m_job)
        return;
    Job& job = *m_job;
    job.writing = false;
    switch (result.status) {
    case WriteStatus::Ok:
        job.partial = result.partial;
        job.next = result.partial.offset;
        job.corruptInARow = 0;
        job.waiting.removeIf([&job](const auto& entry) { return entry.key() < job.next; });
        sendFrame(protocol::SnapshotAck{job.offer->snapshotId, job.next});
        if (job.next == job.partial.packBytes)
            startApply(false);
        else
            pump();
        return;
    case WriteStatus::DiskFull:
        dropPartial();
        fail(QStringLiteral("disk_full"));
        return;
    case WriteStatus::Failed:
        dropPartial();
        fail(QStringLiteral("checkout_failed"));
        return;
    case WriteStatus::Corrupt:
        corrupt();
        return;
    }
}

void SnapshotStoreReceiver::corrupt() {
    Job& job = *m_job;
    clearHeld();
    if (++job.corruptInARow >= maxCorruptInARow) {
        dropPartial();
        fail(QStringLiteral("pack_invalid"));
        return;
    }
    sendFrame(protocol::SnapshotError{job.offer->snapshotId, QStringLiteral("chunk_corrupt")});
}

void SnapshotStoreReceiver::clearHeld() {
    m_job->frames.clear();
    m_job->payloads.clear();
    m_job->waiting.clear();
}

void SnapshotStoreReceiver::dropPartial() {
    const Key current = key();
    m_partials.remove(current);
    work<bool>(
        [this, current] { return m_context.store.removePartial(current.first, current.second); },
        [](quint64, const bool&) {});
}

} // namespace ketplus::runner::receiver
