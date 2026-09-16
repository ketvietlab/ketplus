#include "index/SymbolIndex.h"

#include "editor/SyntaxDefinition.h"
#include "workspace/WorkspaceSearch.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>

#include <algorithm>
#include <limits>

namespace ketplus {
namespace {

// Per symbol: the name, its kind, the line, and the share of the containers holding them.
qint64 symbolBytes(const DocumentSymbol& symbol) {
    constexpr qint64 entryOverhead = 64;
    return entryOverhead + static_cast<qint64>(symbol.name.size() + symbol.kind.size()) * 2;
}

QByteArray contentHash(const QString& text) {
    return QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1);
}

} // namespace

SymbolIndex::SymbolIndex(const qint64 budgetBytes) : budgetBytes_(qMax<qint64>(0, budgetBytes)) {}

void SymbolIndex::addBlobReference(const QByteArray& hash, const QString& text,
                                   const QString& syntax) {
    if (auto known = blobs_.find(hash); known != blobs_.end()) {
        // The same contents are already indexed, from this workspace or another one.
        ++known->references;
        return;
    }

    Blob blob;
    blob.symbols = extractDocumentSymbols(text.toUtf8(), syntax);
    blob.references = 1;
    for (const auto& symbol : blob.symbols) {
        blob.bytes += symbolBytes(symbol);
        auto& owners = names_[symbol.name];
        if (!owners.contains(hash)) {
            owners.append(hash);
        }
    }
    totalBytes_ += blob.bytes;
    blobs_.insert(hash, std::move(blob));
}

void SymbolIndex::indexWorkspace(const QString& root, const WorkspaceFileList& files,
                                 const std::atomic_bool* cancelled) {
    if (root.isEmpty()) {
        return;
    }
    const QDir directory(root);
    Workspace built;

    for (const QString& relativePath : files.relativePaths) {
        if (cancelled != nullptr && cancelled->load()) {
            // Nothing is published, so a cancelled build leaves the old workspace in place.
            const QMutexLocker locker(&mutex_);
            releaseWorkspace(built);
            return;
        }
        if (hasBinaryFileSuffix(relativePath)) {
            continue;
        }
        const QString path = directory.absoluteFilePath(relativePath);
        const QString syntax = QString::fromLatin1(syntaxDefinitionForPath(path).name);
        if (syntax == QStringLiteral("plain")) {
            continue;
        }
        QString text;
        if (!readSearchableFile(path, &text)) {
            continue;
        }
        const QByteArray hash = contentHash(text);
        {
            // Reading happens outside the lock; only publishing one file holds it.
            const QMutexLocker locker(&mutex_);
            addBlobReference(hash, text, syntax);
            built.files[hash].append(relativePath);
        }
    }

    const QMutexLocker locker(&mutex_);
    built.lastUsedMs = QDateTime::currentMSecsSinceEpoch();
    built.lastUsedSequence = ++useSequence_;
    if (auto previous = workspaces_.find(root); previous != workspaces_.end()) {
        releaseWorkspace(*previous);
        workspaces_.erase(previous);
    }
    workspaces_.insert(root, std::move(built));
    enforceBudget(root);
}

bool SymbolIndex::hasWorkspace(const QString& root) const {
    const QMutexLocker locker(&mutex_);
    return workspaces_.contains(root);
}

qint64 SymbolIndex::memoryBytes() const {
    const QMutexLocker locker(&mutex_);
    return totalBytes_;
}

int SymbolIndex::workspaceCount() const {
    const QMutexLocker locker(&mutex_);
    return static_cast<int>(workspaces_.size());
}

int SymbolIndex::blobCount() const {
    const QMutexLocker locker(&mutex_);
    return static_cast<int>(blobs_.size());
}

QList<SymbolHit> SymbolIndex::lookup(const QString& root, const QString& name) {
    const QMutexLocker locker(&mutex_);
    auto workspace = workspaces_.find(root);
    if (workspace == workspaces_.end() || name.isEmpty()) {
        return {};
    }
    workspace->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
    workspace->lastUsedSequence = ++useSequence_;

    QList<SymbolHit> hits;
    for (const QByteArray& hash : names_.value(name)) {
        const auto paths = workspace->files.constFind(hash);
        if (paths == workspace->files.constEnd()) {
            continue;
        }
        const auto blob = blobs_.constFind(hash);
        if (blob == blobs_.constEnd()) {
            continue;
        }
        for (const QString& path : paths.value()) {
            for (const auto& symbol : blob->symbols) {
                if (symbol.name == name) {
                    hits.append({path, symbol.kind, symbol.line});
                }
            }
        }
    }
    std::sort(hits.begin(), hits.end(), [](const SymbolHit& left, const SymbolHit& right) {
        return left.relativePath == right.relativePath ? left.line < right.line
                                                       : left.relativePath < right.relativePath;
    });
    return hits;
}

void SymbolIndex::releaseWorkspace(Workspace& workspace) {
    for (auto file = workspace.files.cbegin(); file != workspace.files.cend(); ++file) {
        auto blob = blobs_.find(file.key());
        if (blob == blobs_.end()) {
            continue;
        }
        blob->references -= static_cast<int>(file.value().size());
        if (blob->references > 0) {
            // Another workspace holds the same contents, so the symbols stay.
            continue;
        }
        totalBytes_ -= blob->bytes;
        for (const auto& symbol : blob->symbols) {
            auto owners = names_.find(symbol.name);
            if (owners != names_.end()) {
                owners->removeAll(file.key());
                if (owners->isEmpty()) {
                    names_.erase(owners);
                }
            }
        }
        blobs_.erase(blob);
    }
    workspace.files.clear();
}

void SymbolIndex::forgetWorkspace(const QString& root) {
    const QMutexLocker locker(&mutex_);
    forgetWorkspaceLocked(root);
}

void SymbolIndex::forgetWorkspaceLocked(const QString& root) {
    auto workspace = workspaces_.find(root);
    if (workspace == workspaces_.end()) {
        return;
    }
    releaseWorkspace(*workspace);
    workspaces_.erase(workspace);
}

int SymbolIndex::evictIdle(const qint64 idleMs, const QString& keepRoot) {
    const QMutexLocker locker(&mutex_);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList stale;
    for (auto workspace = workspaces_.cbegin(); workspace != workspaces_.cend(); ++workspace) {
        if (workspace.key() != keepRoot && now - workspace.value().lastUsedMs > idleMs) {
            stale.append(workspace.key());
        }
    }
    for (const QString& root : stale) {
        forgetWorkspaceLocked(root);
    }
    return static_cast<int>(stale.size());
}

void SymbolIndex::enforceBudget(const QString& keepRoot) {
    while (totalBytes_ > budgetBytes_ && workspaces_.size() > 1) {
        QString oldest;
        quint64 oldestUse = std::numeric_limits<quint64>::max();
        for (auto workspace = workspaces_.cbegin(); workspace != workspaces_.cend(); ++workspace) {
            if (workspace.key() == keepRoot) {
                continue;
            }
            if (workspace.value().lastUsedSequence < oldestUse) {
                oldestUse = workspace.value().lastUsedSequence;
                oldest = workspace.key();
            }
        }
        if (oldest.isEmpty()) {
            break;
        }
        forgetWorkspaceLocked(oldest);
    }
}

void SymbolIndex::clear() {
    const QMutexLocker locker(&mutex_);
    blobs_.clear();
    names_.clear();
    workspaces_.clear();
    totalBytes_ = 0;
}

QStringList SymbolIndex::roots() const {
    const QMutexLocker locker(&mutex_);
    QStringList result = workspaces_.keys();
    result.sort();
    return result;
}

int SymbolIndex::fileCount(const QString& root) const {
    const QMutexLocker locker(&mutex_);
    const auto workspace = workspaces_.constFind(root);
    if (workspace == workspaces_.constEnd()) {
        return 0;
    }
    int count = 0;
    for (const auto& paths : workspace->files) {
        count += static_cast<int>(paths.size());
    }
    return count;
}

} // namespace ketplus
