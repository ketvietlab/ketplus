#pragma once

#include "protocol/Frames.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

// The ports a snapshot sender works through. The sender never opens a connection, reads the
// clock or touches a file itself: the peer session sends what it emits, and long work (building
// a pack, reading a chunk) runs elsewhere and answers on the event loop.
namespace ketplus::daemon::runner::sync {

namespace protocol = ketplus::runner::protocol;

// Codes the sender ends with besides the runner's own `snapshot.error` codes.
namespace code {
inline const QString packUnreadable = QStringLiteral("pack_unreadable");
inline const QString snapshotBuildFailed = QStringLiteral("snapshot_build_failed");
inline const QString snapshotIncomplete = QStringLiteral("snapshot_incomplete");
inline const QString snapshotTooLarge = QStringLiteral("snapshot_too_large");
inline const QString packInvalid = QStringLiteral("pack_invalid");
inline const QString storageError = QStringLiteral("storage_error");
} // namespace code

// One pack file ready to offer.
struct PackFile {
    QString path;
    qint64 size = 0;
    QString sha256; // lowercase hex
    QString snapshotId;
    QString baseId; // empty for a full pack
};

// Outcome of building a pack: the file, or why not. `baseMissing` asks for a full pack instead;
// it is set whenever a pack on a base could not be built.
struct PackOutcome {
    std::optional<PackFile> pack;
    bool baseMissing = false;
    QString code; // set when there is no pack and the base is not the reason
};

// One chunk read from a pack file, with its SHA-256; empty `bytes` when the read failed.
struct ChunkRead {
    QByteArray bytes;
    QString sha256;
};

// Frames and bulk transfers to the runner of the current connection.
class SyncLink {
  public:
    virtual ~SyncLink() = default;
    // A `snapshot.*` frame on `control`; the session adds the epoch.
    virtual void send(const protocol::Frame& frame) = 0;
    // A fresh 16-character lowercase hex id for the next bulk transfer of this connection.
    virtual QString nextTransferId() = 0;
    // False while the bulk channel cannot take another transfer; the session then calls the
    // sender's bulkWritable() once it can.
    [[nodiscard]] virtual bool bulkWritable() const = 0;
    virtual void sendBulk(const QString& transferId, const QByteArray& bytes) = 0;
};

// Work that must not run on the event loop. Each callback is invoked on the event loop, at most
// once; a sender that moved on ignores a late answer.
class SyncWork {
  public:
    virtual ~SyncWork() = default;
    using PackCallback = std::function<void(const PackOutcome&)>;
    using ChunkCallback = std::function<void(const ChunkRead&)>;
    virtual void buildPack(const QString& snapshotId, const QString& baseId,
                           PackCallback callback) = 0;
    virtual void readChunk(const PackFile& pack, qint64 offset, qint64 length,
                           ChunkCallback callback) = 0;
};

// What the store keeps for the sender: the snapshot each runner confirmed per workspace key, and
// the pack of each job in flight. Calls are synchronous; those that change a ref are short and
// limited in time.
class SyncRetention {
  public:
    virtual ~SyncRetention() = default;
    [[nodiscard]] virtual std::optional<QString> confirmed(const QString& runnerId,
                                                           const QString& workspaceKey) const = 0;
    // Durable before it returns true.
    virtual bool confirm(const QString& runnerId, const QString& workspaceKey,
                         const QString& snapshotId) = 0;
    virtual void forget(const QString& runnerId, const QString& workspaceKey) = 0;

    // The pack kept for `jobId`, when it is still on disk.
    [[nodiscard]] virtual std::optional<PackFile> pack(const QString& jobId) const = 0;
    virtual void keepPack(const QString& jobId, const QString& runnerId, const PackFile& pack) = 0;
    // Deletes the job's pack; its in-flight snapshot stays held.
    virtual void dropPack(const QString& jobId) = 0;
    // Deletes the job's pack and releases its in-flight snapshot.
    virtual void release(const QString& jobId) = 0;
};

// What the sender reports upward, on the event loop. Each job reports at most one of them.
class SyncEvents {
  public:
    virtual ~SyncEvents() = default;
    virtual void snapshotReady(const QString& jobId) = 0;
    virtual void snapshotFailed(const QString& jobId, const QString& code) = 0;
};

} // namespace ketplus::daemon::runner::sync
