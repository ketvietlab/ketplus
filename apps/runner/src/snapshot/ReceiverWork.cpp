#include "snapshot/ReceiverWork.h"

#include "snapshot/WorkspaceApply.h"
#include "snapshot/WorkspaceStore.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ketplus::runner::receiver {
namespace {

using snapshot::Error;
using snapshot::ErrorCode;

constexpr qint64 maxPackBytes = qint64(4) * 1024 * 1024 * 1024;

// Codes git or the library return only before the workspace is touched.
bool untouched(const ErrorCode code) {
    return code != ErrorCode::Io && code != ErrorCode::GitFailed;
}

} // namespace

WriteStatus writeAndSync(const QString& path, const qint64 offset, const QByteArray& data) {
    const int fd = ::open(QFile::encodeName(path).constData(),
                          O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return errno == ENOSPC || errno == EDQUOT ? WriteStatus::DiskFull : WriteStatus::Failed;
    qsizetype written = 0;
    int failure = 0;
    while (written < data.size()) {
        const ssize_t count = ::pwrite(fd, data.constData() + written,
                                       size_t(data.size() - written), off_t(offset + written));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            failure = count < 0 ? errno : EIO;
            break;
        }
        written += count;
    }
    if (failure == 0 && ::fsync(fd) != 0)
        failure = errno;
    if (::close(fd) != 0 && failure == 0)
        failure = errno;
    if (failure == 0)
        return WriteStatus::Ok;
    return failure == ENOSPC || failure == EDQUOT ? WriteStatus::DiskFull : WriteStatus::Failed;
}

ReceiverWorker::ReceiverWorker() {
    m_object = new QObject;
    m_object->moveToThread(&m_thread);
    QObject::connect(&m_thread, &QThread::finished, m_object, &QObject::deleteLater);
    m_thread.start();
}

ReceiverWorker::~ReceiverWorker() {
    m_thread.quit();
    m_thread.wait();
}

void ReceiverWorker::post(std::function<void()> job) {
    QMetaObject::invokeMethod(m_object, std::move(job), Qt::QueuedConnection);
}

ProbeResult probe(const WorkContext& context, const QString& repositoryKey,
                  const QString& workspaceKey, const QString& snapshotId,
                  const std::optional<QString>& baseId, const QString& packSha256,
                  const std::optional<PartialRecord>& partial, const bool orphan) {
    ProbeResult result;
    if (orphan) {
        context.store.removeRepository(repositoryKey);
        result.removedOrphan = true;
    }
    const QString mirror = context.store.mirrorPath(repositoryKey);
    const auto have = snapshot::mirrorHasSnapshot(mirror, snapshotId, context.git);
    if (!have) {
        result.error = have.error();
        return result;
    }
    result.haveSnapshot = have.value();
    if (!result.haveSnapshot && baseId) {
        const auto base = snapshot::mirrorHasCommit(mirror, *baseId, context.git);
        if (!base) {
            result.error = base.error();
            return result;
        }
        result.haveBase = base.value();
    }
    if (!partial)
        return result;
    if (result.haveSnapshot || partial->packSha256 != packSha256) {
        context.store.removePartial(repositoryKey, workspaceKey);
        return result;
    }
    // Resume from what is on disk: the smaller of the recorded offset and the file.
    PartialRecord resumed = *partial;
    const QString pack = context.store.partialPackPath(repositoryKey, workspaceKey);
    const qint64 size = QFileInfo(pack).isFile() ? QFileInfo(pack).size() : 0;
    resumed.offset = qMin(resumed.offset, size);
    QString error;
    if ((size > resumed.offset && !QFile::resize(pack, resumed.offset)) ||
        !context.store.write(resumed, &error)) {
        context.store.removePartial(repositoryKey, workspaceKey);
        return result;
    }
    result.partial = resumed;
    return result;
}

bool evict(const WorkContext& context, const QList<Eviction>& evictions) {
    bool ok = true;
    for (const Eviction& eviction : evictions) {
        const WorkspaceRecord& workspace = eviction.workspace;
        const auto removed = snapshot::removeWorkspace(
            context.store.mirrorPath(workspace.repositoryKey),
            context.store.workspacePath(workspace.repositoryKey, workspace.workspaceKey),
            context.git);
        ok = removed.ok() &&
             context.store.removeWorkspaceRecord(workspace.repositoryKey, workspace.workspaceKey) &&
             ok;
        context.store.removePartial(workspace.repositoryKey, workspace.workspaceKey);
        if (eviction.repositoryToo)
            ok = context.store.removeRepository(workspace.repositoryKey) && ok;
    }
    return ok;
}

std::optional<PartialRecord> startPack(const WorkContext& context,
                                       const RepositoryRecord& repository,
                                       const QString& workspaceKey, const QString& packSha256,
                                       const qint64 packBytes, std::optional<PartialRecord> resumed,
                                       QString* error) {
    if (!context.store.write(repository, error))
        return std::nullopt;
    if (resumed)
        return resumed;
    PartialRecord record{
        repository.repositoryKey, workspaceKey, packSha256, packBytes, 0, context.now()};
    if (!context.store.write(record, error))
        return std::nullopt;
    QFile pack(context.store.partialPackPath(repository.repositoryKey, workspaceKey));
    if (!pack.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("cannot create the partial pack");
        context.store.removePartial(repository.repositoryKey, workspaceKey);
        return std::nullopt;
    }
    return record;
}

ChunkResult writeChunk(const WorkContext& context, PartialRecord partial, const QByteArray& data,
                       const qint64 bytes, const QString& sha256) {
    if (context.hold)
        context.hold(WorkStep::Write);
    ChunkResult result{WriteStatus::Ok, partial};
    if (data.size() != bytes ||
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() != sha256.toLatin1()) {
        result.status = WriteStatus::Corrupt;
        return result;
    }
    const QString pack = context.store.partialPackPath(partial.repositoryKey, partial.workspaceKey);
    result.status = context.writer(pack, partial.offset, data);
    if (result.status != WriteStatus::Ok)
        return result;
    partial.offset += bytes;
    partial.lastWrite = context.now();
    QString error;
    if (!context.store.write(partial, &error))
        result.status = WriteStatus::Failed;
    result.partial = partial;
    return result;
}

ApplyOutcome apply(const WorkContext& context, const ApplyInput& input) {
    const QString& repositoryKey = input.repository.repositoryKey;
    const QString mirror = context.store.mirrorPath(repositoryKey);
    const QString workspace = context.store.workspacePath(repositoryKey, input.workspaceKey);
    const QString pack = context.store.partialPackPath(repositoryKey, input.workspaceKey);
    ApplyOutcome outcome;
    const auto fail = [&](const ErrorCode code, const QString& message) {
        outcome.error = Error{code, message};
        outcome.record = input.previous;
        return outcome;
    };
    // A workspace left half-applied, or without a record, starts over.
    std::optional<WorkspaceRecord> previous = input.previous;
    if ((previous && !previous->complete) || (!previous && QFileInfo::exists(workspace))) {
        if (const auto removed = snapshot::removeWorkspace(mirror, workspace, context.git);
            !removed)
            return fail(removed.error().code, removed.error().message);
        previous.reset();
        context.store.removeWorkspaceRecord(repositoryKey, input.workspaceKey);
        outcome.record.reset();
    }
    if (!input.fromMirror && QFileInfo(pack).size() != input.packBytes) {
        context.store.removePartial(repositoryKey, input.workspaceKey);
        outcome.error = Error{ErrorCode::PackInvalid, QStringLiteral("the pack size differs")};
        outcome.record = previous;
        return outcome;
    }

    WorkspaceRecord applying{repositoryKey,
                             input.workspaceKey,
                             input.owner,
                             previous ? previous->snapshotId : QString(),
                             false,
                             context.now(),
                             previous ? previous->bytes : 0};
    QString error;
    if (!context.store.write(input.repository, &error) || !context.store.write(applying, &error)) {
        outcome.error = Error{ErrorCode::Io, error};
        outcome.record = previous;
        return outcome;
    }
    const auto restore = [&] {
        if (previous)
            (void)context.store.write(*previous, &error);
        else
            context.store.removeWorkspaceRecord(repositoryKey, input.workspaceKey);
        outcome.record = previous;
    };
    if (context.hold)
        context.hold(WorkStep::Apply);
    if (input.cancelled->load()) {
        restore();
        outcome.cancelled = true;
        return outcome;
    }

    snapshot::ApplyLimits limits;
    limits.maxPackBytes = maxPackBytes;
    const snapshot::Result<snapshot::Applied> applied =
        input.fromMirror
            ? snapshot::applyFromMirror({mirror, workspace, input.commit, limits, input.clean},
                                        context.git)
            : snapshot::applySnapshot({mirror, workspace, pack, input.packSha256.toLatin1(),
                                       input.commit, limits, input.clean},
                                      context.git);
    if (context.hold)
        context.hold(WorkStep::Applied);
    if (!input.fromMirror)
        context.store.removePartial(repositoryKey, input.workspaceKey);
    outcome.cancelled = input.cancelled->load();
    if (!applied) {
        outcome.error = applied.error();
        if (untouched(applied.error().code))
            restore();
        else
            outcome.record = applying; // half-applied: never held, removed before the next use
        return outcome;
    }
    if (outcome.cancelled) {
        outcome.record = applying;
        return outcome;
    }
    WorkspaceRecord complete = applying;
    complete.snapshotId = input.commit;
    complete.complete = true;
    complete.lastUsed = context.now();
    if (!context.store.write(complete, &error)) {
        outcome.error = Error{ErrorCode::Io, error};
        outcome.record = applying;
        return outcome;
    }
    outcome.record = complete;
    return outcome;
}

StoredRecords tidy(const WorkContext& context, const qint64 keepMs) {
    StoredRecords records = context.store.load();
    const QDateTime now = context.now();
    for (const QString& orphan : std::as_const(records.orphanRepositories))
        context.store.removeRepository(orphan);
    records.orphanRepositories.clear();
    QList<PartialRecord> kept;
    for (const PartialRecord& partial : std::as_const(records.partials)) {
        if (partial.lastWrite.msecsTo(now) > keepMs)
            context.store.removePartial(partial.repositoryKey, partial.workspaceKey);
        else
            kept.append(partial);
    }
    records.partials = kept;
    QString error;
    QList<RepositoryRecord> repositories;
    for (RepositoryRecord repository : std::as_const(records.repositories)) {
        const auto used = [&](const auto& list) {
            return std::any_of(list.cbegin(), list.cend(), [&](const auto& entry) {
                return entry.repositoryKey == repository.repositoryKey;
            });
        };
        if (!used(records.workspaces) && !used(records.partials)) {
            context.store.removeRepository(repository.repositoryKey);
            continue;
        }
        const qint64 bytes = diskBytes(context.store.mirrorPath(repository.repositoryKey));
        if (bytes != repository.mirrorBytes) {
            repository.mirrorBytes = bytes;
            (void)context.store.write(repository, &error);
        }
        repositories.append(repository);
    }
    records.repositories = repositories;
    for (WorkspaceRecord& workspace : records.workspaces) {
        const qint64 bytes =
            diskBytes(context.store.workspacePath(workspace.repositoryKey, workspace.workspaceKey));
        if (bytes != workspace.bytes) {
            workspace.bytes = bytes;
            (void)context.store.write(workspace, &error);
        }
    }
    return records;
}

qint64 diskBytes(const QString& path) {
    qint64 total = 0;
    QDirIterator it(path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        struct stat status{};
        if (::lstat(QFile::encodeName(it.next()).constData(), &status) == 0 &&
            S_ISREG(status.st_mode))
            total += qint64(status.st_size);
    }
    return total;
}

} // namespace ketplus::runner::receiver
