#include "runner/sync/SnapshotWorker.h"

#include "snapshot/SnapshotStore.h"
#include "snapshot/WorktreeSnapshot.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>

#include <utility>

namespace ketplus::daemon::runner::sync {

namespace snapshot = ketplus::runner::snapshot;

namespace {

// Delivers `answer` on the context's thread unless the context is gone by then.
template <typename Answer> void answerOn(QObject* context, Answer answer) {
    QMetaObject::invokeMethod(context, std::move(answer), Qt::QueuedConnection);
}

} // namespace

SnapshotWorker::SnapshotWorker(QString storePath, PackCache& packs, snapshot::GitTool tool)
    : storePath_(std::move(storePath)), packs_(packs), tool_(std::move(tool)),
      context_(std::make_unique<QObject>()) {
    store_.setMaxThreadCount(1);
    chunks_.setMaxThreadCount(1);
}

SnapshotWorker::~SnapshotWorker() {
    store_.clear();
    chunks_.clear();
    store_.waitForDone();
    chunks_.waitForDone();
    context_.reset();
}

void SnapshotWorker::buildPack(const QString& jobId, const QString& snapshotId,
                               const QString& baseId, SyncWork::PackCallback callback) {
    snapshot::PackRequest request{storePath_, snapshotId, baseId,
                                  packs_.pathFor(jobId, snapshotId, baseId)};
    const QString directory = QFileInfo(request.outputPath).path();
    QObject* context = context_.get();
    packs_.setInFlight(jobId, true);
    // A job released while its pack was being built leaves no file behind.
    auto deliver = [&packs = packs_, jobId, callback = std::move(callback)](PackOutcome outcome) {
        if (outcome.pack && !packs.inFlight(jobId))
            QFile::remove(outcome.pack->path);
        callback(outcome);
    };
    store_.start([request, directory, tool = tool_, context, deliver = std::move(deliver)] {
        PackOutcome outcome;
        if (!QDir().mkpath(directory)) {
            outcome.code = code::snapshotBuildFailed;
        } else if (const auto written = snapshot::writePack(request, tool)) {
            outcome.pack = PackFile{written.value().path, written.value().size,
                                    QString::fromLatin1(written.value().sha256),
                                    written.value().commit, written.value().baseCommit};
        } else if (written.error().code == snapshot::ErrorCode::BaseMissing ||
                   !request.baseCommit.isEmpty()) {
            QFile::remove(request.outputPath);
            outcome.baseMissing = true;
        } else {
            QFile::remove(request.outputPath);
            const auto complete =
                snapshot::snapshotComplete(request.storePath, request.commit, tool);
            outcome.code = complete && !complete.value() ? code::snapshotIncomplete
                                                         : code::snapshotBuildFailed;
        }
        answerOn(context, [deliver, outcome] { deliver(outcome); });
    });
}

// A job released while a thread had its pack open could not delete it there: Windows refuses to
// remove an open file. The pack is removed again once the thread is done and has answered.
void SnapshotWorker::removeIfReleased(const QString& jobId) {
    if (!packs_.inFlight(jobId))
        packs_.remove(jobId);
}

void SnapshotWorker::readChunk(const QString& jobId, const PackFile& pack, const qint64 offset,
                               const qint64 length, SyncWork::ChunkCallback callback) {
    QObject* context = context_.get();
    chunks_.start([this, jobId, pack, offset, length, context, callback = std::move(callback)] {
        const ChunkRead read = readPackChunk(pack, offset, length);
        answerOn(context, [this, jobId, callback, read] {
            callback(read);
            removeIfReleased(jobId);
        });
    });
}

void SnapshotWorker::hashPack(const QString& jobId, const PackFile& pack,
                              SyncWork::HashCallback callback) {
    QObject* context = context_.get();
    store_.start([this, jobId, pack, context, callback = std::move(callback)] {
        const QString sha256 = hashPackFile(pack);
        answerOn(context, [this, jobId, callback, sha256] {
            callback(sha256);
            removeIfReleased(jobId);
        });
    });
}

void SnapshotWorker::runOnStore(std::function<void()> work) { store_.start(std::move(work)); }

JobSyncWork::JobSyncWork(SnapshotWorker& worker, QString jobId)
    : worker_(worker), jobId_(std::move(jobId)) {}

void JobSyncWork::buildPack(const QString& snapshotId, const QString& baseId,
                            PackCallback callback) {
    worker_.buildPack(jobId_, snapshotId, baseId, std::move(callback));
}

void JobSyncWork::readChunk(const PackFile& pack, const qint64 offset, const qint64 length,
                            ChunkCallback callback) {
    worker_.readChunk(jobId_, pack, offset, length, std::move(callback));
}

void JobSyncWork::hashPack(const PackFile& pack, HashCallback callback) {
    worker_.hashPack(jobId_, pack, std::move(callback));
}

ChunkRead readPackChunk(const PackFile& pack, const qint64 offset, const qint64 length) {
    if (offset < 0 || length <= 0 || offset + length > pack.size)
        return {};
    QFile file(pack.path);
    if (QFileInfo(pack.path).isSymLink() || !file.open(QIODevice::ReadOnly) ||
        file.size() != pack.size || !file.seek(offset))
        return {};
    QByteArray bytes = file.read(length);
    if (bytes.size() != length)
        return {};
    const QString sha256 =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    return {std::move(bytes), sha256};
}

QString hashPackFile(const PackFile& pack) {
    QFile file(pack.path);
    if (QFileInfo(pack.path).isSymLink() || !file.open(QIODevice::ReadOnly) ||
        file.size() != pack.size)
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || file.pos() != pack.size)
        return {};
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace ketplus::daemon::runner::sync
