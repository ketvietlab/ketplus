#pragma once

#include "runner/sync/SnapshotSender.h"

#include <QCryptographicHash>
#include <QHash>
#include <QList>

#include <deque>
#include <functional>
#include <map>

// In-memory ports for the snapshot sender: a link that records frames and transfers, work whose
// answers the test releases, and a retention map.
namespace snapshot_sync_test {

using namespace ketplus::daemon::runner::sync;

inline constexpr qint64 chunk = protocol::limits::chunkBytes;
inline const QString snapshotA = QString(40, QLatin1Char('a'));
inline const QString snapshotB = QString(40, QLatin1Char('b'));
inline const QString snapshotC = QString(40, QLatin1Char('c'));
inline const QString jobId = QStringLiteral("job-1");
inline const QString runnerId = QStringLiteral("runner-a");
inline const QString workspace = QStringLiteral("wkwkwkwkwkwkwkwkwkwkwkwkwkwkwkwk");

// Bytes of a synthetic pack named by `seed`: a 251-byte period, so every offset differs from
// its neighbours and a chunk placed at the wrong offset changes the digest of the whole. Long
// enough for a whole pack of up to three chunks.
inline QByteArray packBytes(const qint64 offset, const qint64 length, const int seed) {
    static const QByteArray pattern = [] {
        QByteArray bytes(3 * protocol::limits::chunkBytes + 251, Qt::Uninitialized);
        for (qsizetype i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<char>(i % 251);
        return bytes;
    }();
    return pattern.mid((offset + seed) % 251, length);
}

inline QString sha256(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

class FakeLink final : public SyncLink {
  public:
    void send(const protocol::Frame& frame) override { frames.append(frame); }
    QString nextTransferId() override {
        return QStringLiteral("%1").arg(++transfers, 16, 16, QLatin1Char('0'));
    }
    [[nodiscard]] bool bulkWritable() const override { return writable; }
    void sendBulk(const QString& transferId, const QByteArray& bytes) override {
        bulk.append({transferId, bytes});
    }

    template <typename T> [[nodiscard]] QList<T> of() const {
        QList<T> found;
        for (const auto& frame : frames)
            if (const auto* value = std::get_if<T>(&frame))
                found.append(*value);
        return found;
    }

    QList<protocol::Frame> frames;
    QList<std::pair<QString, QByteArray>> bulk;
    quint64 transfers = 0;
    bool writable = true;
};

class FakeWork final : public SyncWork {
  public:
    struct Build {
        QString snapshotId;
        QString baseId;
        PackCallback callback;
    };
    struct Read {
        PackFile pack;
        qint64 offset;
        qint64 length;
        ChunkCallback callback;
    };
    struct Hash {
        PackFile pack;
        HashCallback callback;
    };

    void buildPack(const QString& snapshotId, const QString& baseId,
                   PackCallback callback) override {
        builds.push_back({snapshotId, baseId, std::move(callback)});
        ++buildCount;
    }
    void readChunk(const PackFile& pack, const qint64 offset, const qint64 length,
                   ChunkCallback callback) override {
        reads.push_back({pack, offset, length, std::move(callback)});
        ++readCount;
        if (autoRead)
            finishReads();
    }

    void hashPack(const PackFile& pack, HashCallback callback) override {
        hashes.push_back({pack, std::move(callback)});
        ++hashCount;
    }
    // Answers the oldest hash with the pack's own digest, or with `sha256` when given.
    void finishHash(const std::optional<QString>& sha256 = std::nullopt) {
        Hash hash = std::move(hashes.front());
        hashes.pop_front();
        hash.callback(sha256.value_or(hash.pack.sha256));
    }

    // Answers the oldest build with a synthetic pack of `size` bytes.
    PackFile finishBuild(const qint64 size, const int seed = 1) {
        Build build = std::move(builds.front());
        builds.pop_front();
        PackFile pack;
        pack.path = QStringLiteral("/packs/%1-%2.pack").arg(build.snapshotId).arg(seed);
        pack.size = size;
        pack.sha256 = sha256(QByteArray::number(seed) + build.snapshotId.toLatin1());
        pack.snapshotId = build.snapshotId;
        pack.baseId = build.baseId;
        seeds[pack.path] = seed;
        build.callback(PackOutcome{pack, false, {}});
        return pack;
    }
    void failBuild(const PackOutcome& outcome) {
        Build build = std::move(builds.front());
        builds.pop_front();
        build.callback(outcome);
    }
    // Answers the reads asked so far, not those their answers start.
    void finishReads() {
        for (auto count = reads.size(); count > 0 && !reads.empty(); --count) {
            Read read = std::move(reads.front());
            reads.pop_front();
            if (failRead) {
                read.callback(ChunkRead{});
                continue;
            }
            QByteArray bytes = packBytes(read.offset, read.length, seeds.value(read.pack.path));
            if (shortRead)
                bytes.chop(1);
            read.callback(ChunkRead{bytes, sha256(bytes)});
        }
    }

    std::deque<Build> builds;
    std::deque<Read> reads;
    std::deque<Hash> hashes;
    QHash<QString, int> seeds;
    int buildCount = 0;
    int readCount = 0;
    int hashCount = 0;
    bool autoRead = true;
    bool failRead = false;
    bool shortRead = false;
};

class FakeRetention final : public SyncRetention {
  public:
    [[nodiscard]] std::optional<QString> confirmed(const QString& runner,
                                                   const QString& key) const override {
        const auto found = confirmedRefs.find({runner, key});
        return found == confirmedRefs.end() ? std::nullopt : std::optional(found->second);
    }
    bool confirm(const QString& runner, const QString& key, const QString& snapshot) override {
        if (failConfirm)
            return false;
        confirmedRefs[{runner, key}] = snapshot;
        log.append(QStringLiteral("confirm"));
        return true;
    }
    void forget(const QString& runner, const QString& key) override {
        confirmedRefs.erase({runner, key});
        log.append(QStringLiteral("forget"));
    }
    [[nodiscard]] std::optional<PackFile> pack(const QString& job) const override {
        const auto found = packs.constFind(job);
        return found == packs.cend() ? std::nullopt : std::optional(*found);
    }
    void keepPack(const QString& job, const QString&, const PackFile& pack) override {
        packs.insert(job, pack);
    }
    void dropPack(const QString& job) override {
        packs.remove(job);
        log.append(QStringLiteral("drop"));
    }
    void release(const QString& job) override {
        packs.remove(job);
        released.append(job);
        log.append(QStringLiteral("release"));
    }

    std::map<std::pair<QString, QString>, QString> confirmedRefs;
    QHash<QString, PackFile> packs;
    QStringList released;
    QStringList log;
    bool failConfirm = false;
};

class FakeEvents final : public SyncEvents {
  public:
    void snapshotReady(const QString& job) override { ready.append(job); }
    void snapshotFailed(const QString& job, const QString& code) override {
        failed.append({job, code});
    }
    QStringList ready;
    QList<std::pair<QString, QString>> failed;
};

struct Harness {
    FakeLink link;
    FakeWork work;
    FakeRetention retention;
    FakeEvents events;
    SenderLimits limits;
    std::unique_ptr<SnapshotSender> sender;

    explicit Harness(const QString& snapshot = snapshotA,
                     const QStringList& warnings = {QStringLiteral("lfs_not_synced")}) {
        renew(snapshot, warnings);
    }
    void renew(const QString& snapshot = snapshotA, const QStringList& warnings = {}) {
        sender = std::make_unique<SnapshotSender>(
            SyncJob{jobId, runnerId, workspace, snapshot, warnings}, link, work, retention, events,
            limits);
    }
    // Started and offered a pack of `size` bytes.
    PackFile offered(const qint64 size, const QMap<QString, QString>& hello = {}) {
        sender->start(hello);
        return work.finishBuild(size);
    }
    void want(const QString& state, const qint64 from = 0, const QString& snapshot = snapshotA) {
        sender->receive(protocol::SnapshotWant{snapshot, state, from});
    }
    void ack(const qint64 offset, const QString& snapshot = snapshotA) {
        sender->receive(protocol::SnapshotAck{snapshot, offset});
    }
    void error(const QString& code, const QString& snapshot = snapshotA) {
        sender->receive(protocol::SnapshotError{snapshot, code});
    }
    void ready(const QString& snapshot = snapshotA) {
        sender->receive(protocol::SnapshotReady{snapshot});
    }
    [[nodiscard]] qint64 inFlight() const { return sender->sent() - sender->acked(); }
};

} // namespace snapshot_sync_test
