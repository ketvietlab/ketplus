#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QProcessEnvironment>
#include <QStringList>

#include <functional>

class QObject;

namespace ketplus::daemon {

struct WorkspaceFileScope final {
    QString ownerSubjectId;
    QString workspaceId;
    QString machineId;
    QString repositoryId;
    QString worktreeId;
    QString rootPath;
};

// Read-only, revision-fenced access to a registered worktree. Public replies never contain
// rootPath.
class ReadOnlyWorkspaceFiles final {
  public:
    static constexpr int maximumTreeEntries = 200;
    static constexpr int maximumSearchResults = 100;
    static constexpr qsizetype maximumFileBytes = 512 * 1024;
    static constexpr qsizetype maximumDiffBytes = 512 * 1024;
    static constexpr qsizetype maximumSearchFileBytes = 512 * 1024;
    // Dirty files larger than this are fingerprinted by metadata (size, mtime, ctime, inode)
    // instead of content, and content hashing stops once the per-revision budget is spent.
    static constexpr qint64 maximumRevisionHashFileBytes = 1024 * 1024;
    static constexpr qint64 maximumRevisionHashTotalBytes = 32 * 1024 * 1024;

    // Hardened argument prefix and environment for every daemon git invocation: a repository's
    // own config must not be able to run commands (core.fsmonitor) or take locks.
    [[nodiscard]] static QStringList hardenedGitArguments(const QString& worktree);
    [[nodiscard]] static QProcessEnvironment hardenedGitEnvironment();

    // Live recomputes the revision with git before and after every read. Published trusts a
    // request revision the caller already matched to one it published for this worktree, and
    // serves the worktree as it is now under that revision.
    enum class RevisionFence { Live, Published };

    explicit ReadOnlyWorkspaceFiles(WorkspaceFileScope scope,
                                    RevisionFence fence = RevisionFence::Live);

    // Blocking: spawns git and reads files. Never call on the daemon's main thread; use
    // revisionAsync() there.
    [[nodiscard]] QString revision(QString* errorCode = nullptr) const;
    using RevisionCallback = std::function<void(const QString& revision, const QString& error)>;
    // Computes revision() on QThreadPool::globalInstance() and invokes callback on context's
    // thread. The callback is dropped if context is destroyed first.
    static void revisionAsync(WorkspaceFileScope scope, QObject* context,
                              RevisionCallback callback);
    [[nodiscard]] QJsonObject tree(const QJsonObject& request) const;
    [[nodiscard]] QJsonObject search(const QJsonObject& request) const;
    [[nodiscard]] QJsonObject file(const QJsonObject& request) const;
    [[nodiscard]] QJsonObject diff(const QJsonObject& request) const;
    // The bytes of one image beneath the root, for a phone that opened it from the file tree.
    // Not revision-fenced: an image is shown as it is on disk now, and the reply is a preview,
    // never a basis for an edit. Needs only scope.rootPath. Returns an error code, or empty.
    static constexpr qsizetype maximumImageBytes = 16 * 1024 * 1024;
    [[nodiscard]] QString readImage(const QString& relativePath, QByteArray* bytes) const;

  private:
    [[nodiscard]] bool scopeMatches(const QJsonObject& request) const;
    [[nodiscard]] QString resolved(const QString& relativePath, QString* errorCode) const;
    [[nodiscard]] QJsonObject fencedError(const QString& code) const;
    [[nodiscard]] QByteArray git(const QStringList& arguments, int maximumBytes,
                                 QString* errorCode) const;
    [[nodiscard]] QString startRevision(const QString& expected) const;
    [[nodiscard]] bool finishRevision(const QString& expected, QJsonObject* result) const;

    WorkspaceFileScope scope_;
    RevisionFence fence_;
    QString canonicalRoot_;
};

} // namespace ketplus::daemon
