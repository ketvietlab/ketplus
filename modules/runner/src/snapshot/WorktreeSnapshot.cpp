#include "snapshot/WorktreeSnapshot.h"

#include "snapshot/RepositoryLayout.h"
#include "snapshot/UserGitRules.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

namespace ketplus::runner::snapshot {
namespace {

// Identity and date are fixed so the same tree always gives the same commit id.
const QStringList snapshotIdentity{
    QStringLiteral("GIT_AUTHOR_NAME=KetPlus snapshot"),
    QStringLiteral("GIT_AUTHOR_EMAIL=snapshot@ketplus.invalid"),
    QStringLiteral("GIT_AUTHOR_DATE=@1 +0000"),
    QStringLiteral("GIT_COMMITTER_NAME=KetPlus snapshot"),
    QStringLiteral("GIT_COMMITTER_EMAIL=snapshot@ketplus.invalid"),
    QStringLiteral("GIT_COMMITTER_DATE=@1 +0000"),
};

QString storeObjectFormat(const QString& storePath) {
    QFile config(QDir(storePath).filePath(QStringLiteral("config")));
    if (!config.open(QIODevice::ReadOnly))
        return {};
    const QString text = QString::fromUtf8(config.readAll()).toLower();
    return text.contains(QStringLiteral("objectformat = sha256")) ? QStringLiteral("sha256")
                                                                  : QStringLiteral("sha1");
}

std::optional<Error> writeFileAtomically(const QString& path, const QByteArray& content) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() ||
        !file.commit()) {
        return Error{ErrorCode::Io, QStringLiteral("cannot write %1").arg(path)};
    }
    return std::nullopt;
}

QString sourceObjectsFile() { return QStringLiteral("ketplus-source-objects"); }

// The read-only view of the repository's objects for a command that writes nothing to it.
QString readAlternate(const QString& store) {
    QFile file(QDir(store).filePath(sourceObjectsFile()));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QStringLiteral("GIT_ALTERNATE_OBJECT_DIRECTORIES=") + QString::fromUtf8(file.readAll());
}

// Creates the store on first use and binds it to the repository's object directory.
std::optional<Error> prepareStore(const SealedGit& git, const QString& storePath,
                                  const RepositoryLayout& layout) {
    if (!QDir::isAbsolutePath(storePath))
        return Error{ErrorCode::InvalidArgument, QStringLiteral("store path must be absolute")};
    const QString storeRoot = QDir::cleanPath(storePath);
    const auto inside = [](const QString& path, const QString& root) {
        return path == root || path.startsWith(root + QLatin1Char('/'));
    };
    // Never let the store sit inside the worktree or the git directory it reads.
    for (const QString& root : {layout.workTree, layout.gitDir, layout.commonDir}) {
        if (inside(storeRoot, root) || inside(QFileInfo(storeRoot).canonicalFilePath(), root)) {
            return Error{ErrorCode::InvalidArgument,
                         QStringLiteral("the store must live outside the repository")};
        }
    }
    if (!QFileInfo::exists(QDir(storeRoot).filePath(QStringLiteral("HEAD")))) {
        if (!QDir().mkpath(storeRoot))
            return Error{ErrorCode::Io, QStringLiteral("cannot create %1").arg(storeRoot)};
        if (const auto init = git.initBare(storeRoot, layout.objectFormat); !init)
            return init.error();
    } else if (storeObjectFormat(storeRoot) != layout.objectFormat) {
        return Error{ErrorCode::StoreMismatch,
                     QStringLiteral("the store uses another object format")};
    }
    // Bound by a file of our own rather than objects/info/alternates: with a standing
    // alternate, git "freshens" (touches the mtime of) any repository object or pack it is
    // about to write again, which would change the user's repository. Commands that only read
    // get the repository's objects through GIT_ALTERNATE_OBJECT_DIRECTORIES instead.
    if (layout.objectsDir.contains(QLatin1Char('\n')) ||
        layout.objectsDir.contains(QLatin1Char(':')))
        return Error{ErrorCode::UnsupportedRepository,
                     QStringLiteral("unsupported character in %1").arg(layout.objectsDir)};
    const QString binding = QDir(storeRoot).filePath(sourceObjectsFile());
    const QByteArray wanted = layout.objectsDir.toUtf8();
    QFile existing(binding);
    if (existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly))
            return Error{ErrorCode::Io, QStringLiteral("cannot read %1").arg(binding)};
        if (existing.readAll() != wanted) {
            return Error{ErrorCode::StoreMismatch,
                         QStringLiteral("the store belongs to another repository")};
        }
    } else if (auto error = writeFileAtomically(binding, wanted)) {
        return error;
    }
    // The repository's info/exclude and info/attributes, copied as data into the store's own
    // info directory, where the snapshot's git reads them; .gitignore and .gitattributes
    // files are read in place and the user's own files are given per snapshot.
    QDir().mkpath(QDir(storeRoot).filePath(QStringLiteral("info")));
    for (const QString& name :
         {QStringLiteral("info/exclude"), QStringLiteral("info/attributes")}) {
        const auto content = readRulesFile(QDir(layout.commonDir).filePath(name));
        if (!content)
            return content.error();
        if (auto error = writeFileAtomically(QDir(storeRoot).filePath(name), content.value()))
            return error;
    }
    return std::nullopt;
}

// A copy of the worktree's index (and any split-index shared files), so the stat cache makes
// `git add` cheap while the user's own index is never opened for writing.
std::optional<Error> copyIndex(const RepositoryLayout& layout, const QString& storePath,
                               const QString& target) {
    if (!QFileInfo::exists(layout.indexFile))
        return std::nullopt;
    if (!QFile::copy(layout.indexFile, target))
        return Error{ErrorCode::Io, QStringLiteral("cannot copy %1").arg(layout.indexFile)};
    QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner);
    const QDir gitDir(layout.gitDir);
    for (const QString& shared : gitDir.entryList({QStringLiteral("sharedindex.*")}, QDir::Files)) {
        const QString destination = QDir(storePath).filePath(shared);
        if (!QFileInfo::exists(destination) && !QFile::copy(gitDir.filePath(shared), destination))
            return Error{ErrorCode::Io, QStringLiteral("cannot copy %1").arg(shared)};
    }
    return std::nullopt;
}

QStringList splitNul(const QByteArray& output) {
    QStringList items;
    for (const QByteArray& item : output.split('\0')) {
        if (!item.isEmpty())
            items << QString::fromUtf8(item);
    }
    return items;
}

QString oneLine(const QByteArray& output) { return QString::fromUtf8(output).trimmed(); }

} // namespace

QByteArray fileSha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return hash.result().toHex();
}

Result<Snapshot> createSnapshot(const SnapshotRequest& request, const GitTool& tool) {
    const SealedGit git(tool);
    if (const auto version = git.checkVersion(); !version)
        return version.error();
    const auto layout = readRepositoryLayout(request.worktreePath);
    if (!layout)
        return layout.error();
    if (auto error = prepareStore(git, request.storePath, layout.value()))
        return *error;
    const auto userRules = readUserGitRules(
        git, layout.value(),
        request.userGitSources ? *request.userGitSources : userGitSourcesFromEnvironment());
    if (!userRules)
        return userRules.error();
    const QString store = QDir::cleanPath(request.storePath);

    QTemporaryDir scratch(QDir(store).filePath(QStringLiteral("snapshot-XXXXXX")));
    if (!scratch.isValid())
        return Error{ErrorCode::Io, QStringLiteral("cannot create a scratch directory")};
    const QString index = scratch.filePath(QStringLiteral("index"));
    if (auto error = copyIndex(layout.value(), store, index))
        return *error;
    // The user's excludes and attributes files at their own low precedence, in place of the
    // sealed /dev/null.
    const QString userExcludes = scratch.filePath(QStringLiteral("excludes"));
    const QString userAttributes = scratch.filePath(QStringLiteral("attributes"));
    if (auto error = writeFileAtomically(userExcludes, userRules.value().excludes))
        return *error;
    if (auto error = writeFileAtomically(userAttributes, userRules.value().attributes))
        return *error;

    GitRun run;
    run.gitDir = store;
    run.workTree = layout.value().workTree;
    run.indexFile = index;
    run.extraOptions = {QStringLiteral("core.excludesFile=") + userExcludes,
                        QStringLiteral("core.attributesFile=") + userAttributes};
    // `filter` unspecified (!) or explicitly unset (-): the only paths whose worktree content
    // may enter the snapshot.
    const QString unfiltered = QStringLiteral(":(top,attr:!filter)");
    const QString unsetFilter = QStringLiteral(":(top,attr:-filter)");

    // Stage into the scratch index. Filtered paths are left out, so a tracked one keeps its
    // index blob and an untracked one is never hashed.
    run.arguments = {QStringLiteral("add"), QStringLiteral("--all"), QStringLiteral("--"),
                     unfiltered, unsetFilter};
    if (const auto added = git.run(run); !added)
        return added.error();

    const auto listFiles = [&](const QStringList& pathspecs) -> Result<QStringList> {
        GitRun list = run;
        list.arguments =
            QStringList{QStringLiteral("ls-files"),           QStringLiteral("-z"),
                        QStringLiteral("--cached"),           QStringLiteral("--others"),
                        QStringLiteral("--exclude-standard"), QStringLiteral("--")} +
            pathspecs;
        const auto listed = git.run(list);
        if (!listed)
            return listed.error();
        QStringList paths = splitNul(listed.value().out);
        paths.removeDuplicates();
        return paths;
    };
    Snapshot snapshot;
    const auto lfsFiles = listFiles({QStringLiteral(":(top,attr:filter=lfs)")});
    if (!lfsFiles)
        return lfsFiles.error();
    snapshot.lfsPaths = lfsFiles.value();
    const auto filteredFiles =
        listFiles({QStringLiteral(":(top)"), QStringLiteral(":(top,exclude,attr:!filter)"),
                   QStringLiteral(":(top,exclude,attr:-filter)")});
    if (!filteredFiles)
        return filteredFiles.error();
    const QSet<QString> lfs(snapshot.lfsPaths.cbegin(), snapshot.lfsPaths.cend());
    for (const QString& path : filteredFiles.value()) {
        if (!lfs.contains(path))
            snapshot.filteredPaths << path;
    }

    run.arguments = {QStringLiteral("ls-files"), QStringLiteral("-z"), QStringLiteral("--stage")};
    const auto staged = git.run(run);
    if (!staged)
        return staged.error();
    for (const QString& entry : splitNul(staged.value().out)) {
        if (entry.startsWith(QStringLiteral("160000 ")))
            snapshot.submodulePaths << entry.section(QLatin1Char('\t'), 1);
    }

    // `add` above and `write-tree` here run without the repository's objects in view, so they
    // never rewrite (and so freshen) one of them; unchanged entries are trusted by id.
    run.arguments = {QStringLiteral("write-tree"), QStringLiteral("--missing-ok")};
    const auto tree = git.run(run);
    if (!tree)
        return tree.error();
    snapshot.tree = oneLine(tree.value().out);
    if (!isObjectId(snapshot.tree))
        return Error{ErrorCode::GitFailed, QStringLiteral("git returned no tree id")};

    // That write-tree reuses the copied index's cache-tree, and skips writing any tree the
    // cache-tree still holds valid (whether it does depends on which entries `add` refreshed).
    // Those trees exist only in the repository, so rebuild the tree from its own listing into
    // an index without a cache-tree: write-tree then writes every tree into the store.
    const QString alternate = readAlternate(store);
    GitRun listTree;
    listTree.gitDir = store;
    listTree.extraEnvironment = {alternate};
    listTree.arguments = {QStringLiteral("ls-tree"), QStringLiteral("-r"), QStringLiteral("-z"),
                          QStringLiteral("--full-tree"), snapshot.tree};
    const auto listing = git.run(listTree);
    if (!listing)
        return listing.error();
    GitRun rebuild;
    rebuild.gitDir = store;
    rebuild.indexFile = scratch.filePath(QStringLiteral("tree-index"));
    rebuild.arguments = {QStringLiteral("update-index"), QStringLiteral("-z"),
                         QStringLiteral("--index-info")};
    rebuild.input = listing.value().out;
    if (const auto rebuilt = git.run(rebuild); !rebuilt)
        return rebuilt.error();
    rebuild.arguments = {QStringLiteral("write-tree"), QStringLiteral("--missing-ok")};
    rebuild.input.clear();
    const auto written = git.run(rebuild);
    if (!written)
        return written.error();
    if (oneLine(written.value().out) != snapshot.tree)
        return Error{ErrorCode::SnapshotIncomplete,
                     QStringLiteral("the snapshot tree changed when written to the store")};

    GitRun commit;
    commit.gitDir = store;
    commit.extraEnvironment = snapshotIdentity;
    // commit-tree checks the tree exists; the commit it writes is new, never a repository one.
    commit.extraEnvironment << alternate;
    commit.arguments = {QStringLiteral("commit-tree"), snapshot.tree, QStringLiteral("-m"),
                        QStringLiteral("KetPlus worktree snapshot")};
    const auto committed = git.run(commit);
    if (!committed)
        return committed.error();
    snapshot.commit = oneLine(committed.value().out);
    if (!isObjectId(snapshot.commit))
        return Error{ErrorCode::GitFailed, QStringLiteral("git returned no commit id")};

    // Every object of the snapshot must be readable from the store or the repository before
    // it is handed out; a pack written from an incomplete one would fail on the runner.
    GitRun complete;
    complete.gitDir = store;
    complete.extraEnvironment = {alternate};
    complete.arguments = {QStringLiteral("rev-list"), QStringLiteral("--quiet"),
                          QStringLiteral("--objects"), QStringLiteral("--missing=error"),
                          snapshot.commit};
    if (const auto checked = git.run(complete); !checked) {
        return Error{ErrorCode::SnapshotIncomplete,
                     QStringLiteral("the snapshot is missing objects: ") + checked.error().message};
    }
    return snapshot;
}

Result<Pack> writePack(const PackRequest& request, const GitTool& tool) {
    if (!isObjectId(request.commit) ||
        (!request.baseCommit.isEmpty() && !isObjectId(request.baseCommit))) {
        return Error{ErrorCode::InvalidArgument, QStringLiteral("malformed snapshot id")};
    }
    if (request.outputPath.isEmpty() || !QDir::isAbsolutePath(request.outputPath))
        return Error{ErrorCode::InvalidArgument, QStringLiteral("pack path must be absolute")};
    const QString store = QDir::cleanPath(request.storePath);
    if (!QFileInfo::exists(QDir(store).filePath(QStringLiteral("HEAD"))))
        return Error{ErrorCode::InvalidArgument,
                     QStringLiteral("no snapshot store at %1").arg(store)};
    const SealedGit git(tool);
    if (const auto version = git.checkVersion(); !version)
        return version.error();

    const auto isCommit = [&](const QString& id) {
        GitRun check;
        check.gitDir = store;
        check.arguments = {QStringLiteral("cat-file"), QStringLiteral("-t"), id};
        const auto type = git.run(check);
        return type && oneLine(type.value().out) == QStringLiteral("commit");
    };
    if (!isCommit(request.commit))
        return Error{ErrorCode::InvalidArgument,
                     QStringLiteral("unknown snapshot %1").arg(request.commit)};
    if (!request.baseCommit.isEmpty() && !isCommit(request.baseCommit))
        return Error{ErrorCode::BaseMissing,
                     QStringLiteral("base %1 is gone").arg(request.baseCommit)};

    GitRun pack;
    pack.gitDir = store;
    pack.arguments = {QStringLiteral("pack-objects"), QStringLiteral("--revs"),
                      QStringLiteral("--stdout"), QStringLiteral("--quiet")};
    pack.extraEnvironment = {readAlternate(store)};
    pack.input = request.commit.toLatin1() + '\n';
    // Snapshots have no parents, so excluding the base commit alone would not exclude its
    // objects; excluding its tree does.
    if (!request.baseCommit.isEmpty())
        pack.input += '^' + request.baseCommit.toLatin1() + "^{tree}\n";
    pack.outputFile = request.outputPath;
    if (const auto written = git.run(pack); !written) {
        QFile::remove(request.outputPath);
        return written.error();
    }

    Pack result;
    result.path = request.outputPath;
    result.size = QFileInfo(request.outputPath).size();
    result.sha256 = fileSha256(request.outputPath);
    result.commit = request.commit;
    result.baseCommit = request.baseCommit;
    if (result.sha256.isEmpty())
        return Error{ErrorCode::Io, QStringLiteral("cannot read %1").arg(request.outputPath)};
    return result;
}

} // namespace ketplus::runner::snapshot
