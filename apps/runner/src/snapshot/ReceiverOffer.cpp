#include "protocol/FrameLimits.h"
#include "snapshot/SnapshotStoreReceiver.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace ketplus::runner::receiver {
namespace {

using protocol::SnapshotError;
using protocol::SnapshotWant;
using snapshot::ErrorCode;

constexpr qsizetype maxHeld = protocol::limits::maxSnapshots;

bool nested(const QString& a, const QString& b) {
    return a == b || a.startsWith(b + QLatin1Char('/')) || b.startsWith(a + QLatin1Char('/'));
}

QString canonical(const QString& path) {
    const QString resolved = QFileInfo(path).canonicalFilePath();
    return resolved.isEmpty() ? QDir::cleanPath(path) : resolved;
}

QString codeFor(const ErrorCode code) {
    switch (code) {
    case ErrorCode::PackTooLarge:
    case ErrorCode::CheckoutTooLarge:
        return QStringLiteral("snapshot_too_large");
    case ErrorCode::PackDigestMismatch:
    case ErrorCode::PackInvalid:
    case ErrorCode::SnapshotIncomplete:
    case ErrorCode::UnsafePath:
    case ErrorCode::InvalidArgument:
        return QStringLiteral("pack_invalid");
    default:
        return QStringLiteral("checkout_failed");
    }
}

} // namespace

std::unique_ptr<SnapshotStoreReceiver>
SnapshotStoreReceiver::create(const app::StateDirectory& state, const QString& workRoot,
                              ReceiverOptions options, QString* error) {
    if (!QDir::isAbsolutePath(workRoot) || nested(canonical(state.path()), canonical(workRoot))) {
        *error = QStringLiteral("The work directory must be absolute and apart from the state "
                                "directory.");
        return nullptr;
    }
    return std::unique_ptr<SnapshotStoreReceiver>(
        new SnapshotStoreReceiver(state, workRoot, std::move(options)));
}

SnapshotStoreReceiver::SnapshotStoreReceiver(const app::StateDirectory& state,
                                             const QString& workRoot, ReceiverOptions options)
    : m_store(state, workRoot), m_options(std::move(options)),
      m_context{m_store,       m_options.git,  m_options.writer,
                m_options.now, m_options.hold, m_options.limits} {
    adopt(m_store.load());
    const qint64 keep = m_options.partialKeepMs;
    work<StoredRecords>([this, keep] { return tidy(m_context, keep); },
                        [this](quint64, const StoredRecords& records) { adopt(records); });
}

SnapshotStoreReceiver::~SnapshotStoreReceiver() {
    if (m_job)
        m_job->cancelled->store(true);
}

void SnapshotStoreReceiver::answerTo(Ready ready, Failed failed) {
    m_ready = std::move(ready);
    m_failed = std::move(failed);
}

QMap<QString, QString> SnapshotStoreReceiver::heldSnapshots(const QString& machineId) const {
    QList<const WorkspaceRecord*> held;
    for (const WorkspaceRecord& record : m_workspaces)
        if (record.complete && record.owner == machineId &&
            QFileInfo(m_store.workspacePath(record.repositoryKey, record.workspaceKey)).isDir())
            held.append(&record);
    std::sort(held.begin(), held.end(),
              [](const auto* a, const auto* b) { return a->lastUsed > b->lastUsed; });
    QMap<QString, QString> snapshots;
    for (const WorkspaceRecord* record : held.mid(0, maxHeld))
        snapshots.insert(record->workspaceKey, record->snapshotId);
    return snapshots;
}

void SnapshotStoreReceiver::begin(const protocol::JobSpec& spec, const QString& machineId,
                                  Send send) {
    cancel();
    ++m_generation;
    m_job.emplace();
    m_job->spec = spec;
    m_job->machineId = machineId;
    m_job->send = std::move(send);
    if (!ReceiverStore::isKey(spec.repositoryKey) || !ReceiverStore::isKey(spec.workspaceKey)) {
        fail(QStringLiteral("checkout_failed"));
        return;
    }
    m_inUse = key();
}

void SnapshotStoreReceiver::receive(const protocol::Frame& frame) {
    if (const auto* offer = std::get_if<protocol::SnapshotOffer>(&frame))
        onOffer(*offer);
    else if (const auto* chunk = std::get_if<protocol::SnapshotChunk>(&frame))
        onChunk(*chunk);
}

void SnapshotStoreReceiver::cancel() {
    if (!m_job)
        return;
    m_job->cancelled->store(true);
    m_job.reset();
    ++m_generation;
}

void SnapshotStoreReceiver::idle() {
    m_inUse.reset();
    const qint64 keep = m_options.partialKeepMs;
    work<StoredRecords>(
        [this, keep] { return tidy(m_context, keep); },
        [this](quint64, const StoredRecords& records) {
            adopt(records);
            const std::optional<Key> keepKey = m_job ? std::optional(key()) : m_inUse;
            const auto evictions = evictionsFor(0, keepKey);
            if (!evictions || evictions->isEmpty())
                return;
            const QList<Eviction> list = *evictions;
            for (const Eviction& eviction : list)
                forget({eviction.workspace.repositoryKey, eviction.workspace.workspaceKey});
            work<bool>([this, list] { return evict(m_context, list); },
                       [](quint64, const bool&) {});
        });
}

qint64 SnapshotStoreReceiver::usedBytes() const {
    qint64 total = 0;
    for (const RepositoryRecord& repository : m_repositories)
        total += repository.mirrorBytes;
    for (const WorkspaceRecord& workspace : m_workspaces)
        total += workspace.bytes;
    for (const PartialRecord& partial : m_partials)
        total += partial.offset;
    return total;
}

// The least recently used workspaces whose removal brings the stored total plus `needed`
// within budget, never `keep`; none when that cannot be done.
std::optional<QList<Eviction>>
SnapshotStoreReceiver::evictionsFor(const qint64 needed, const std::optional<Key>& keep) const {
    qint64 over = usedBytes() + needed - m_options.budgetBytes;
    QList<Eviction> evictions;
    if (over <= 0)
        return evictions;
    QList<WorkspaceRecord> candidates;
    for (const WorkspaceRecord& record : m_workspaces)
        if (!keep || Key{record.repositoryKey, record.workspaceKey} != *keep)
            candidates.append(record);
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.complete != b.complete ? !a.complete : a.lastUsed < b.lastUsed;
    });
    QMap<QString, int> left;
    for (const WorkspaceRecord& record : m_workspaces)
        ++left[record.repositoryKey];
    for (const WorkspaceRecord& record : candidates) {
        Eviction eviction{record, false};
        over -= record.bytes + m_partials.value({record.repositoryKey, record.workspaceKey}).offset;
        if (--left[record.repositoryKey] == 0 && (!keep || keep->first != record.repositoryKey)) {
            eviction.repositoryToo = true;
            over -= m_repositories.value(record.repositoryKey).mirrorBytes;
        }
        evictions.append(eviction);
        if (over <= 0)
            return evictions;
    }
    return std::nullopt;
}

void SnapshotStoreReceiver::onOffer(const protocol::SnapshotOffer& offer) {
    if (!m_job)
        return;
    Job& job = *m_job;
    if (offer.jobId != job.spec.jobId || offer.snapshotId != job.spec.snapshotId) {
        fail(QStringLiteral("pack_invalid"));
        return;
    }
    if (job.phase == Phase::Applying)
        return; // its answer comes
    job.offer = offer;
    const Key current = key();
    if ((m_repositories.contains(current.first) &&
         m_repositories.value(current.first).owner != job.machineId) ||
        (m_workspaces.contains(current) && m_workspaces.value(current).owner != job.machineId)) {
        fail(QStringLiteral("checkout_failed"));
        return;
    }
    if (offer.packBytes > protocol::limits::maxPackBytes) {
        fail(QStringLiteral("snapshot_too_large"));
        return;
    }
    clearHeld();
    job.corruptInARow = 0;
    if (job.phase == Phase::Receiving && offer.packSha256 == job.partial.packSha256 &&
        offer.packBytes == job.partial.packBytes) {
        sendFrame(SnapshotWant{offer.snapshotId, QStringLiteral("need"), job.next});
        return;
    }
    job.phase = Phase::Probing;
    job.writing = false;
    ++m_generation;
    const std::optional<PartialRecord> partial =
        m_partials.contains(current) ? std::optional(m_partials.value(current)) : std::nullopt;
    const bool orphan = m_orphans.contains(current.first);
    work<ProbeResult>(
        [this, current, offer, partial, orphan] {
            return probe(m_context, current.first, current.second, offer.snapshotId, offer.baseId,
                         offer.packSha256, partial, orphan);
        },
        [this, current](const quint64 generation, const ProbeResult& result) {
            if (result.removedOrphan)
                m_orphans.remove(current.first);
            if (result.partial)
                m_partials.insert(current, *result.partial);
            else
                m_partials.remove(current);
            probed(generation, result);
        });
}

void SnapshotStoreReceiver::probed(const quint64 generation, const ProbeResult& result) {
    if (generation != m_generation || !m_job)
        return;
    Job& job = *m_job;
    const protocol::SnapshotOffer& offer = *job.offer;
    if (result.error) {
        fail(QStringLiteral("checkout_failed"));
        return;
    }
    if (result.haveSnapshot) {
        sendFrame(SnapshotWant{offer.snapshotId, QStringLiteral("have"), 0});
        startApply(true);
        return;
    }
    if (offer.baseId && !result.haveBase) {
        sendFrame(SnapshotError{offer.snapshotId, QStringLiteral("base_missing")});
        job.phase = Phase::WaitingOffer;
        return;
    }
    const qint64 resumedAt = result.partial ? result.partial->offset : 0;
    const std::optional<QList<Eviction>> evictions =
        evictionsFor(offer.packBytes - resumedAt, key());
    if (!evictions) {
        fail(QStringLiteral("disk_full"));
        return;
    }
    const Key current = key();
    const RepositoryRecord repository =
        m_repositories.value(current.first, RepositoryRecord{current.first, job.machineId, 0});
    const QList<Eviction> list = *evictions;
    for (const Eviction& eviction : list)
        forget({eviction.workspace.repositoryKey, eviction.workspace.workspaceKey});
    struct Started {
        std::optional<PartialRecord> partial;
    };
    const std::optional<PartialRecord> resumed = result.partial;
    work<Started>(
        [this, list, repository, current, offer, resumed] {
            (void)evict(m_context, list);
            QString error;
            return Started{startPack(m_context, repository, current.second, offer.packSha256,
                                     offer.packBytes, resumed, &error)};
        },
        [this, repository, current](const quint64 generation, const Started& started) {
            m_repositories.insert(current.first, repository);
            if (started.partial)
                m_partials.insert(current, *started.partial);
            if (generation != m_generation || !m_job)
                return;
            if (!started.partial) {
                fail(QStringLiteral("checkout_failed"));
                return;
            }
            receivePack(generation, started.partial);
        });
}

void SnapshotStoreReceiver::receivePack(quint64, const std::optional<PartialRecord>& resumed) {
    Job& job = *m_job;
    job.partial = *resumed;
    job.next = resumed->offset;
    job.phase = Phase::Receiving;
    sendFrame(SnapshotWant{job.spec.snapshotId, QStringLiteral("need"), job.next});
    if (job.next == job.partial.packBytes)
        startApply(false); // every byte was already here
    else
        pump(); // chunks that came early
}

void SnapshotStoreReceiver::startApply(const bool fromMirror) {
    Job& job = *m_job;
    job.phase = Phase::Applying;
    const Key current = key();
    ApplyInput input;
    input.repository =
        m_repositories.value(current.first, RepositoryRecord{current.first, job.machineId, 0});
    input.workspaceKey = current.second;
    input.owner = job.machineId;
    if (m_workspaces.contains(current))
        input.previous = m_workspaces.value(current);
    input.fromMirror = fromMirror;
    input.packSha256 = job.offer->packSha256;
    input.packBytes = job.offer->packBytes;
    input.commit = job.spec.snapshotId;
    input.clean = job.spec.clean;
    input.cancelled = job.cancelled;
    work<ApplyOutcome>(
        [this, input] { return apply(m_context, input); },
        [this, input, current, fromMirror](const quint64 generation, const ApplyOutcome& outcome) {
            m_repositories.insert(current.first, input.repository);
            if (outcome.record)
                m_workspaces.insert(current, *outcome.record);
            else
                m_workspaces.remove(current);
            m_partials.remove(current);
            applied(generation, outcome, fromMirror);
        });
}

void SnapshotStoreReceiver::applied(const quint64 generation, const ApplyOutcome& outcome,
                                    const bool fromMirror) {
    if (generation != m_generation || !m_job || outcome.cancelled)
        return;
    Job& job = *m_job;
    if (!outcome.error) {
        const QString workspace = m_store.workspacePath(key().first, key().second);
        const QStringList warnings = job.offer->warnings;
        m_job.reset();
        const quint64 now = m_generation;
        QMetaObject::invokeMethod(
            &m_home,
            [this, now, workspace, warnings] {
                if (now == m_generation && m_ready)
                    m_ready(workspace, warnings);
            },
            Qt::QueuedConnection);
        return;
    }
    const ErrorCode code = outcome.error->code;
    if (!fromMirror && job.offer->baseId && !job.baseMissingAfterPack &&
        (code == ErrorCode::PackInvalid || code == ErrorCode::SnapshotIncomplete)) {
        job.baseMissingAfterPack = true;
        job.phase = Phase::WaitingOffer;
        sendFrame(SnapshotError{job.spec.snapshotId, QStringLiteral("base_missing")});
        return;
    }
    fail(codeFor(code));
}

void SnapshotStoreReceiver::adopt(const StoredRecords& records) {
    m_repositories.clear();
    m_workspaces.clear();
    m_partials.clear();
    for (const RepositoryRecord& repository : records.repositories)
        m_repositories.insert(repository.repositoryKey, repository);
    for (const WorkspaceRecord& workspace : records.workspaces)
        m_workspaces.insert({workspace.repositoryKey, workspace.workspaceKey}, workspace);
    for (const PartialRecord& partial : records.partials)
        m_partials.insert({partial.repositoryKey, partial.workspaceKey}, partial);
    m_orphans =
        QSet<QString>(records.orphanRepositories.cbegin(), records.orphanRepositories.cend());
}

void SnapshotStoreReceiver::sendFrame(const protocol::Frame& frame) {
    if (m_job && m_job->send)
        m_job->send(frame);
}

void SnapshotStoreReceiver::fail(const QString& code) {
    m_job.reset();
    const quint64 now = m_generation;
    QMetaObject::invokeMethod(
        &m_home,
        [this, now, code] {
            if (now == m_generation && m_failed)
                m_failed(code);
        },
        Qt::QueuedConnection);
}

void SnapshotStoreReceiver::forget(const Key& workspace) {
    m_workspaces.remove(workspace);
    m_partials.remove(workspace);
    bool last = true;
    for (auto it = m_workspaces.cbegin(); it != m_workspaces.cend(); ++it)
        last = last && it.key().first != workspace.first;
    if (last && (!m_job || key().first != workspace.first) &&
        (!m_inUse || m_inUse->first != workspace.first))
        m_repositories.remove(workspace.first);
}

SnapshotStoreReceiver::Key SnapshotStoreReceiver::key() const {
    return {m_job->spec.repositoryKey, m_job->spec.workspaceKey};
}

} // namespace ketplus::runner::receiver
