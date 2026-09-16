#pragma once

#include "editor/SymbolExtractor.h"
#include "workspace/WorkspaceFileIndex.h"

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>

namespace ketplus {

// Where a symbol was declared, relative to the workspace root that holds it.
struct SymbolHit final {
    QString relativePath;
    QString kind;
    int line{0};
};

// Every workspace together may hold this much before the least recently used one is dropped.
inline constexpr qint64 defaultSymbolIndexBudgetBytes = 500LL * 1024 * 1024;
// A workspace untouched for this long is dropped even while the budget has room.
inline constexpr qint64 defaultSymbolIndexIdleMs = 15 * 60 * 1000;

// Declarations of every file in a workspace, so a lookup is a hash probe instead of a scan.
//
// Files are keyed by the hash of their contents, so two workspaces holding the same file --
// two worktrees of one repository, say -- extract and store it once. Workspaces are dropped
// when the total passes the budget, least recently used first, and when left untouched.
//
// Every method takes the same lock, so a build on a worker thread and a lookup on the UI
// thread are safe; a lookup waits for the file being indexed, never for the whole build.
class SymbolIndex final {
  public:
    explicit SymbolIndex(qint64 budgetBytes = defaultSymbolIndexBudgetBytes);

    // Reads and indexes `files` under `root`, replacing what that root held. Safe to call from
    // a worker thread as long as no other call runs at the same time; stops when `cancelled`
    // is set, leaving the workspace untouched.
    void indexWorkspace(const QString& root, const WorkspaceFileList& files,
                        const std::atomic_bool* cancelled = nullptr);
    [[nodiscard]] bool hasWorkspace(const QString& root) const;
    // Re-reads one file, so a lookup after a save does not point at a line that has moved.
    void reindexFile(const QString& root, const QString& relativePath);
    // Declarations of `name` in `root`, marking that workspace as recently used.
    [[nodiscard]] QList<SymbolHit> lookup(const QString& root, const QString& name);
    void forgetWorkspace(const QString& root);
    // Drops workspaces untouched for longer than `idleMs`, except `keepRoot`. Returns how many.
    int evictIdle(qint64 idleMs, const QString& keepRoot = {});
    void clear();

    [[nodiscard]] qint64 memoryBytes() const;
    [[nodiscard]] qint64 budgetBytes() const noexcept { return budgetBytes_; }
    [[nodiscard]] int workspaceCount() const;
    [[nodiscard]] int blobCount() const;
    [[nodiscard]] QStringList roots() const;
    [[nodiscard]] int fileCount(const QString& root) const;

  private:
    struct Blob final {
        QList<DocumentSymbol> symbols;
        qint64 bytes{0};
        int references{0};
    };
    struct Workspace final {
        // Content hash to the paths in this workspace that hold it.
        QHash<QByteArray, QStringList> files;
        qint64 lastUsedMs{0};
        // Wall-clock time ties when two workspaces are touched in the same millisecond, so
        // the order of use is kept separately.
        quint64 lastUsedSequence{0};
    };

    void enforceBudget(const QString& keepRoot);
    void forgetWorkspaceLocked(const QString& root);
    void releaseWorkspace(Workspace& workspace);
    void addBlobReference(const QByteArray& hash, const QString& text, const QString& syntax);

    mutable QMutex mutex_;
    QHash<QByteArray, Blob> blobs_;
    // Symbol name to the blobs that declare it.
    QHash<QString, QList<QByteArray>> names_;
    QHash<QString, Workspace> workspaces_;
    qint64 budgetBytes_;
    qint64 totalBytes_{0};
    quint64 useSequence_{0};
};

} // namespace ketplus
