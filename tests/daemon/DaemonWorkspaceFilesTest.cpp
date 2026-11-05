#include "workspace/ReadOnlyWorkspaceFiles.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QProcess>
#include <QDateTime>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QtTest>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#endif

using ketplus::daemon::ReadOnlyWorkspaceFiles;
using ketplus::daemon::WorkspaceFileScope;

namespace {

void writeFile(const QString& path, const QByteArray& content) {
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
    QCOMPARE(file.write(content), content.size());
}

void git(const QString& root, const QStringList& arguments) {
    QProcess process;
    process.setProgram(QStringLiteral("git"));
    process.setArguments(QStringList{QStringLiteral("-C"), root} + arguments);
    process.start();
    QVERIFY(process.waitForFinished());
    QCOMPARE(process.exitCode(), 0);
}

QJsonObject request(const QString& revision, const QString& path = {}) {
    QJsonObject value{{QStringLiteral("owner_subject_id"), QStringLiteral("owner-1")},
                      {QStringLiteral("workspace_id"), QStringLiteral("workspace-1")},
                      {QStringLiteral("machine_id"), QStringLiteral("machine-1")},
                      {QStringLiteral("repository_id"), QStringLiteral("repository-1")},
                      {QStringLiteral("worktree_id"), QStringLiteral("worktree-1")},
                      {QStringLiteral("revision"), revision}};
    if (!path.isEmpty())
        value.insert(QStringLiteral("path"), path);
    return value;
}

} // namespace

class DaemonWorkspaceFilesTest final : public QObject {
    Q_OBJECT

  private slots:
    void readOnlyOperationsAreRevisionFencedAndBounded();
    void rejectsScopeTraversalSymlinkBinaryOversizedAndStale();
    void searchHasBoundedDeadlineAndHonestTruncation();
    void revisionFingerprintsLargeDirtyFilesByMetadata();
    void gitInvocationsIgnoreRepositoryFsmonitor();
    void gitInvocationsIgnoreRepositoryFilters();
    void searchSkipsIgnoredFolders();
    void ignoredAndSecretFilesAreNotShared();
    void revisionAsyncDeliversOnContextThread();
    void publishedFenceServesTheWorktreeAsItIsNow();
};

namespace {
WorkspaceFileScope scopeFor(const QString& root) {
    return {QStringLiteral("owner-1"),      QStringLiteral("workspace-1"),
            QStringLiteral("machine-1"),    QStringLiteral("repository-1"),
            QStringLiteral("worktree-1"),   root};
}

void initRepository(const QString& root) {
    git(root, {QStringLiteral("init"), QStringLiteral("-q")});
    git(root, {QStringLiteral("config"), QStringLiteral("user.email"),
               QStringLiteral("fixture@example.invalid")});
    git(root, {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Fixture")});
    writeFile(QDir(root).filePath(QStringLiteral("README.md")), "Fixture\n");
    git(root, {QStringLiteral("add"), QStringLiteral(".")});
    git(root, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("fixture")});
}
} // namespace

void DaemonWorkspaceFilesTest::revisionFingerprintsLargeDirtyFilesByMetadata() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    initRepository(root.path());
    const QString large = root.filePath(QStringLiteral("large.bin"));
    QByteArray content(ReadOnlyWorkspaceFiles::maximumRevisionHashFileBytes + 16, 'a');
    writeFile(large, content);
    const ReadOnlyWorkspaceFiles files(scopeFor(root.path()));
    QString error;
    const QString first = files.revision(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!first.isEmpty());
    QCOMPARE(files.revision(), first);

    // Same size, different bytes: detected through mtime/ctime even though content is not read.
    content[0] = 'b';
    writeFile(large, content);
    QFile touched(large);
    QVERIFY(touched.open(QIODevice::ReadWrite));
    QVERIFY(touched.setFileTime(QDateTime::currentDateTimeUtc().addSecs(3600),
                                QFileDevice::FileModificationTime));
    touched.close();
    const QString second = files.revision();
    QVERIFY(!second.isEmpty());
    QVERIFY(second != first);
    QCOMPARE(files.revision(), second);
}

void DaemonWorkspaceFilesTest::gitInvocationsIgnoreRepositoryFsmonitor() {
#if !defined(Q_OS_UNIX)
    QSKIP("fsmonitor hook fixture uses a shell script.");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    initRepository(root.path());
    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    const QString marker = outside.filePath(QStringLiteral("fsmonitor-ran"));
    const QString hook = outside.filePath(QStringLiteral("hook.sh"));
    writeFile(hook, QStringLiteral("#!/bin/sh\ntouch '%1'\nexit 1\n").arg(marker).toUtf8());
    QVERIFY(QFile::setPermissions(hook, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                            QFileDevice::ExeOwner));
    git(root.path(), {QStringLiteral("config"), QStringLiteral("core.fsmonitor"), hook});
    writeFile(root.filePath(QStringLiteral("dirty.txt")), "dirty\n");

    const ReadOnlyWorkspaceFiles files(scopeFor(root.path()));
    QString error;
    const QString revision = files.revision(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!revision.isEmpty());
    QJsonObject modifiedRequest = request(revision);
    modifiedRequest.insert(QStringLiteral("modified_only"), true);
    QVERIFY(files.tree(modifiedRequest).value(QStringLiteral("ok")).toBool());
    QVERIFY(files.diff(request(revision, QStringLiteral("dirty.txt")))
                .value(QStringLiteral("ok"))
                .toBool());
    QVERIFY2(!QFile::exists(marker), "Repository core.fsmonitor was executed by the daemon.");

    // Control: an unhardened status does run the hook, so the assertion above is meaningful.
    QProcess plain;
    plain.start(QStringLiteral("git"),
                {QStringLiteral("-C"), root.path(), QStringLiteral("status"),
                 QStringLiteral("--porcelain")});
    QVERIFY(plain.waitForFinished());
    if (!QFile::exists(marker))
        QWARN("This git build did not invoke core.fsmonitor; control check inconclusive.");
#endif
}

void DaemonWorkspaceFilesTest::gitInvocationsIgnoreRepositoryFilters() {
#if !defined(Q_OS_UNIX)
    QSKIP("filter fixture uses a shell command.");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    initRepository(root.path());
    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    const QString clean = outside.filePath(QStringLiteral("clean-ran"));
    const QString process = outside.filePath(QStringLiteral("process-ran"));
    git(root.path(), {QStringLiteral("config"), QStringLiteral("filter.probe.clean"),
                      QStringLiteral("sh -c 'touch \"%1\"; cat'").arg(clean)});
    git(root.path(), {QStringLiteral("config"), QStringLiteral("filter.probe.process"),
                      QStringLiteral("sh -c 'touch \"%1\"; exit 1'").arg(process)});
    writeFile(root.filePath(QStringLiteral(".git/info/attributes")), "* filter=probe\n");
    // Same bytes, new mtime: status has to rehash the tracked file through the filter.
    QTest::qWait(1100);
    writeFile(root.filePath(QStringLiteral("README.md")), "Fixture\n");

    const ReadOnlyWorkspaceFiles files(scopeFor(root.path()));
    QString error;
    const QString revision = files.revision(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QJsonObject modifiedRequest = request(revision);
    modifiedRequest.insert(QStringLiteral("modified_only"), true);
    QVERIFY(files.tree(modifiedRequest).value(QStringLiteral("ok")).toBool());
    QVERIFY(files.diff(request(revision, QStringLiteral("README.md")))
                .value(QStringLiteral("ok"))
                .toBool());
    QVERIFY2(!QFile::exists(clean) && !QFile::exists(process),
             "A repository filter driver was executed by the daemon.");

    // Control: an unhardened status runs the filter, so the assertion above is meaningful.
    QProcess plain;
    plain.start(QStringLiteral("git"), {QStringLiteral("-C"), root.path(), QStringLiteral("status"),
                                        QStringLiteral("--porcelain")});
    QVERIFY(plain.waitForFinished());
    QVERIFY(QFile::exists(clean) || QFile::exists(process));
#endif
}

void DaemonWorkspaceFilesTest::searchSkipsIgnoredFolders() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    initRepository(root.path());
    writeFile(root.filePath(QStringLiteral(".gitignore")), "node_modules/\n");
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("node_modules/pkg")));
    // More ignored files than the search budget, sorted before src/.
    for (int index = 0; index < 5'100; ++index)
        writeFile(root.filePath(QStringLiteral("node_modules/pkg/f%1.js").arg(index)), "x\n");
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("src")));
    writeFile(root.filePath(QStringLiteral("src/app.ts")), "const needleValue = 1;\n");

    const ReadOnlyWorkspaceFiles files(scopeFor(root.path()));
    const QString revision = files.revision();
    QJsonObject searchRequest = request(revision);
    searchRequest.insert(QStringLiteral("query"), QStringLiteral("needleValue"));
    const QJsonObject result = files.search(searchRequest);
    QVERIFY2(result.value(QStringLiteral("ok")).toBool(),
             qPrintable(QJsonDocument(result).toJson()));
    const QJsonArray matches = result.value(QStringLiteral("matches")).toArray();
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.first().toObject().value(QStringLiteral("path")).toString(),
             QStringLiteral("src/app.ts"));
    QVERIFY(!result.value(QStringLiteral("truncated")).toBool());
}

void DaemonWorkspaceFilesTest::ignoredAndSecretFilesAreNotShared() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    initRepository(root.path());
    writeFile(root.filePath(QStringLiteral(".gitignore")), "local/\n");
    writeFile(root.filePath(QStringLiteral(".env")), "TOKEN=shared-secret\n");
    writeFile(root.filePath(QStringLiteral(".env.example")), "TOKEN=shared-placeholder\n");
    writeFile(root.filePath(QStringLiteral("deploy.pem")), "shared-secret key\n");
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("local")));
    writeFile(root.filePath(QStringLiteral("local/notes.txt")), "shared-secret notes\n");
    writeFile(root.filePath(QStringLiteral("notes.txt")), "shared-public notes\n");

    const ReadOnlyWorkspaceFiles files(scopeFor(root.path()));
    const QString revision = files.revision();
    const QJsonObject tree = files.tree(request(revision));
    QVERIFY(tree.value(QStringLiteral("ok")).toBool());
    QStringList names;
    for (const QJsonValue& entry : tree.value(QStringLiteral("entries")).toArray())
        names.append(entry.toObject().value(QStringLiteral("name")).toString());
    QVERIFY2(names.contains(QStringLiteral(".env.example")), qPrintable(names.join(u',')));
    QVERIFY(names.contains(QStringLiteral("notes.txt")));
    QVERIFY(!names.contains(QStringLiteral(".env")));
    QVERIFY(!names.contains(QStringLiteral("deploy.pem")));
    QVERIFY(!names.contains(QStringLiteral("local")));

    for (const QString& path : {QStringLiteral(".env"), QStringLiteral("deploy.pem"),
                                QStringLiteral("local/notes.txt")}) {
        QCOMPARE(files.file(request(revision, path)).value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("permission_denied"));
        QCOMPARE(files.diff(request(revision, path)).value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("permission_denied"));
    }
    QVERIFY(files.file(request(revision, QStringLiteral(".env.example")))
                .value(QStringLiteral("ok"))
                .toBool());

    QJsonObject searchRequest = request(revision);
    searchRequest.insert(QStringLiteral("query"), QStringLiteral("shared-"));
    QStringList found;
    for (const QJsonValue& match :
         files.search(searchRequest).value(QStringLiteral("matches")).toArray())
        found.append(match.toObject().value(QStringLiteral("path")).toString());
    found.sort();
    QCOMPARE(found, (QStringList{QStringLiteral(".env.example"), QStringLiteral("notes.txt")}));
    QByteArray image;
    QCOMPARE(files.readImage(QStringLiteral("deploy.pem"), &image),
             QStringLiteral("permission_denied"));
}

void DaemonWorkspaceFilesTest::revisionAsyncDeliversOnContextThread() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    initRepository(root.path());
    const QString expected = ReadOnlyWorkspaceFiles(scopeFor(root.path())).revision();
    QVERIFY(!expected.isEmpty());
    QObject context;
    bool delivered = false;
    ReadOnlyWorkspaceFiles::revisionAsync(
        scopeFor(root.path()), &context,
        [&](const QString& revision, const QString& error) {
            QCOMPARE(QThread::currentThread(), context.thread());
            QVERIFY(error.isEmpty());
            QCOMPARE(revision, expected);
            delivered = true;
        });
    QVERIFY(!delivered);
    QTRY_VERIFY_WITH_TIMEOUT(delivered, 10'000);

    // A destroyed context drops the callback instead of invoking it on a dangling object.
    bool dropped = true;
    {
        QObject shortLived;
        ReadOnlyWorkspaceFiles::revisionAsync(scopeFor(root.path()), &shortLived,
                                              [&](const QString&, const QString&) {
                                                  dropped = false;
                                              });
    }
    QTest::qWait(500);
    QThreadPool::globalInstance()->waitForDone(10'000);
    QCoreApplication::processEvents();
    QVERIFY(dropped);
}

void DaemonWorkspaceFilesTest::readOnlyOperationsAreRevisionFencedAndBounded() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    git(root.path(), {QStringLiteral("init"), QStringLiteral("-q")});
    git(root.path(), {QStringLiteral("config"), QStringLiteral("user.email"),
                      QStringLiteral("fixture@example.invalid")});
    git(root.path(),
        {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Fixture")});
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("Sources")));
    writeFile(root.filePath(QStringLiteral("Sources/App.swift")), "let value = 1\nneedle\n");
    writeFile(root.filePath(QStringLiteral("README.md")), "Fixture\n");
    git(root.path(), {QStringLiteral("add"), QStringLiteral(".")});
    git(root.path(), {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("fixture")});
    writeFile(root.filePath(QStringLiteral("Sources/App.swift")), "let value = 2\nneedle\n");
    QVERIFY(QFile::remove(root.filePath(QStringLiteral("README.md"))));

    ReadOnlyWorkspaceFiles files({QStringLiteral("owner-1"), QStringLiteral("workspace-1"),
                                  QStringLiteral("machine-1"), QStringLiteral("repository-1"),
                                  QStringLiteral("worktree-1"), root.path()});
    const QString revision = files.revision();
    QVERIFY(!revision.isEmpty());

    QJsonObject treeRequest = request(revision);
    treeRequest.insert(QStringLiteral("path"), QStringLiteral("Sources"));
    treeRequest.insert(QStringLiteral("limit"), 1);
    const QJsonObject tree = files.tree(treeRequest);
    QVERIFY(tree.value(QStringLiteral("ok")).toBool());
    QCOMPARE(tree.value(QStringLiteral("entries")).toArray().size(), 1);

    const QJsonObject file = files.file(request(revision, QStringLiteral("Sources/App.swift")));
    QVERIFY(file.value(QStringLiteral("ok")).toBool());
    QCOMPARE(file.value(QStringLiteral("content")).toString(),
             QStringLiteral("let value = 2\nneedle\n"));
    QVERIFY(!file.contains(QStringLiteral("root_path")));

    QJsonObject searchRequest = request(revision);
    searchRequest.insert(QStringLiteral("query"), QStringLiteral("needle"));
    searchRequest.insert(QStringLiteral("limit"), 1);
    const QJsonObject search = files.search(searchRequest);
    QVERIFY(search.value(QStringLiteral("ok")).toBool());
    QCOMPARE(search.value(QStringLiteral("matches")).toArray().size(), 1);

    const QJsonObject diff = files.diff(request(revision, QStringLiteral("Sources/App.swift")));
    QVERIFY(diff.value(QStringLiteral("ok")).toBool());
    QVERIFY(diff.value(QStringLiteral("content")).toString().contains(QStringLiteral("value = 2")));

    QJsonObject modifiedRequest = request(revision);
    modifiedRequest.insert(QStringLiteral("path"), QStringLiteral("."));
    modifiedRequest.insert(QStringLiteral("modified_only"), true);
    const QJsonArray modified =
        files.tree(modifiedRequest).value(QStringLiteral("entries")).toArray();
    QHash<QString, QString> changeKinds;
    for (const QJsonValue& value : modified) {
        const QJsonObject entry = value.toObject();
        changeKinds.insert(entry.value(QStringLiteral("path")).toString(),
                           entry.value(QStringLiteral("change_kind")).toString());
    }
    QCOMPARE(changeKinds.value(QStringLiteral("Sources/App.swift")), QStringLiteral("modified"));
    QCOMPARE(changeKinds.value(QStringLiteral("README.md")), QStringLiteral("deleted"));

    writeFile(root.filePath(QStringLiteral("Sources/Untracked.swift")), "let fresh = true\n");
    const QString untrackedRevision = files.revision();
    modifiedRequest.insert(QStringLiteral("revision"), untrackedRevision);
    const QJsonArray withUntracked =
        files.tree(modifiedRequest).value(QStringLiteral("entries")).toArray();
    const auto untrackedEntry =
        std::find_if(withUntracked.cbegin(), withUntracked.cend(), [](const QJsonValue& value) {
            return value.toObject().value(QStringLiteral("path")).toString() ==
                   QStringLiteral("Sources/Untracked.swift");
        });
    QVERIFY(untrackedEntry != withUntracked.cend());
    QCOMPARE(untrackedEntry->toObject().value(QStringLiteral("change_kind")).toString(),
             QStringLiteral("added"));
    const QJsonObject untrackedDiff =
        files.diff(request(untrackedRevision, QStringLiteral("Sources/Untracked.swift")));
    QVERIFY(untrackedDiff.value(QStringLiteral("ok")).toBool());
    QVERIFY(untrackedDiff.value(QStringLiteral("content"))
                .toString()
                .contains(QStringLiteral("new file mode")));
}

void DaemonWorkspaceFilesTest::searchHasBoundedDeadlineAndHonestTruncation() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    git(root.path(), {QStringLiteral("init"), QStringLiteral("-q")});
    git(root.path(), {QStringLiteral("config"), QStringLiteral("user.email"),
                      QStringLiteral("fixture@example.invalid")});
    git(root.path(),
        {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Fixture")});
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("many")));
    for (int index = 0; index < 5'010; ++index) {
        writeFile(root.filePath(QStringLiteral("many/%1.txt").arg(index)), "safe fixture\n");
    }
    git(root.path(), {QStringLiteral("add"), QStringLiteral("many")});
    git(root.path(), {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("fixture")});
    ReadOnlyWorkspaceFiles files({QStringLiteral("owner-1"), QStringLiteral("workspace-1"),
                                  QStringLiteral("machine-1"), QStringLiteral("repository-1"),
                                  QStringLiteral("worktree-1"), root.path()});
    QJsonObject searchRequest = request(files.revision());
    searchRequest.insert(QStringLiteral("query"), QStringLiteral("never-present"));
    QElapsedTimer timer;
    timer.start();
    const QJsonObject result = files.search(searchRequest);
    QVERIFY(result.value(QStringLiteral("ok")).toBool());
    QVERIFY(result.value(QStringLiteral("truncated")).toBool());
    QVERIFY2(timer.elapsed() < 3'000, "Search exceeded its total deadline envelope");
}

void DaemonWorkspaceFilesTest::rejectsScopeTraversalSymlinkBinaryOversizedAndStale() {
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid() && outside.isValid());
    git(root.path(), {QStringLiteral("init"), QStringLiteral("-q")});
    git(root.path(), {QStringLiteral("config"), QStringLiteral("user.email"),
                      QStringLiteral("fixture@example.invalid")});
    git(root.path(),
        {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Fixture")});
    writeFile(root.filePath(QStringLiteral("safe.txt")), "safe\n");
    git(root.path(), {QStringLiteral("add"), QStringLiteral(".")});
    git(root.path(), {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("fixture")});
    writeFile(root.filePath(QStringLiteral("binary.bin")), QByteArray("a\0b", 3));
    writeFile(root.filePath(QStringLiteral("large.txt")),
              QByteArray(ReadOnlyWorkspaceFiles::maximumFileBytes + 1, 'x'));
    writeFile(outside.filePath(QStringLiteral("secret.txt")), "secret\n");
    QVERIFY(QFile::link(outside.filePath(QStringLiteral("secret.txt")),
                        root.filePath(QStringLiteral("link.txt"))));

    ReadOnlyWorkspaceFiles files({QStringLiteral("owner-1"), QStringLiteral("workspace-1"),
                                  QStringLiteral("machine-1"), QStringLiteral("repository-1"),
                                  QStringLiteral("worktree-1"), root.path()});
    const QString revision = files.revision();
    QJsonObject wrong = request(revision, QStringLiteral("safe.txt"));
    wrong.insert(QStringLiteral("machine_id"), QStringLiteral("other"));
    QCOMPARE(files.file(wrong).value(QStringLiteral("error_code")).toString(),
             QStringLiteral("scope_forbidden"));
    QCOMPARE(files.file(request(revision, QStringLiteral("../secret.txt")))
                 .value(QStringLiteral("error_code"))
                 .toString(),
             QStringLiteral("invalid_path"));
    QCOMPARE(files.file(request(revision, QStringLiteral("nested/.git/config")))
                 .value(QStringLiteral("error_code"))
                 .toString(),
             QStringLiteral("invalid_path"));
    QCOMPARE(files.file(request(revision, QStringLiteral("link.txt")))
                 .value(QStringLiteral("error_code"))
                 .toString(),
             QStringLiteral("symlink_unsupported"));
    // Case-insensitive file systems resolve these to the repository's .git directory.
    for (const QString& gitPath : {QStringLiteral(".GIT/config"), QStringLiteral(".Git/HEAD")}) {
        QCOMPARE(files.file(request(revision, gitPath)).value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("invalid_path"));
    }
    // A linked directory in the middle of the path must not be followed out of the worktree.
    QVERIFY(QFile::link(outside.path(), root.filePath(QStringLiteral("linkdir"))));
    QCOMPARE(files.file(request(files.revision(), QStringLiteral("linkdir/secret.txt")))
                 .value(QStringLiteral("error_code"))
                 .toString(),
             QStringLiteral("symlink_unsupported"));
#if defined(Q_OS_UNIX)
    // Opening a FIFO must fail immediately instead of blocking the daemon thread.
    const QByteArray fifo = QFile::encodeName(root.filePath(QStringLiteral("pipe")));
    QCOMPARE(::mkfifo(fifo.constData(), 0600), 0);
    QElapsedTimer fifoTimer;
    fifoTimer.start();
    QCOMPARE(files.file(request(files.revision(), QStringLiteral("pipe")))
                 .value(QStringLiteral("error_code"))
                 .toString(),
             QStringLiteral("permission_denied"));
    QVERIFY(fifoTimer.elapsed() < 5'000);
    QVERIFY(QFile::remove(root.filePath(QStringLiteral("pipe"))));
#endif
    QVERIFY(QFile::remove(root.filePath(QStringLiteral("linkdir"))));
    QCOMPARE(files.file(request(revision, QStringLiteral("binary.bin")))
                 .value(QStringLiteral("error_code"))
                 .toString(),
             QStringLiteral("binary"));
    QCOMPARE(files.file(request(revision, QStringLiteral("large.txt")))
                 .value(QStringLiteral("error_code"))
                 .toString(),
             QStringLiteral("oversized"));
    writeFile(root.filePath(QStringLiteral("safe.txt")), "evil\n");
    QCOMPARE(files.file(request(revision, QStringLiteral("safe.txt")))
                 .value(QStringLiteral("error_code"))
                 .toString(),
             QStringLiteral("stale_revision"));
}

void DaemonWorkspaceFilesTest::publishedFenceServesTheWorktreeAsItIsNow() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    initRepository(root.path());
    const QString published = ReadOnlyWorkspaceFiles(scopeFor(root.path())).revision();
    QVERIFY(!published.isEmpty());
    // An agent edits the file after the revision the phone saw was published.
    writeFile(root.filePath(QStringLiteral("README.md")), "Edited\n");

    const ReadOnlyWorkspaceFiles live(scopeFor(root.path()));
    QCOMPARE(live.file(request(published, QStringLiteral("README.md")))
                 .value(QStringLiteral("error_code"))
                 .toString(),
             QStringLiteral("stale_revision"));

    const ReadOnlyWorkspaceFiles fenced(scopeFor(root.path()),
                                        ReadOnlyWorkspaceFiles::RevisionFence::Published);
    const QJsonObject file = fenced.file(request(published, QStringLiteral("README.md")));
    QVERIFY(file.value(QStringLiteral("ok")).toBool());
    QCOMPARE(file.value(QStringLiteral("revision")).toString(), published);
    QVERIFY(file.value(QStringLiteral("content")).toString().contains(QStringLiteral("Edited")));
    const QJsonObject diff = fenced.diff(request(published, QStringLiteral("README.md")));
    QVERIFY(diff.value(QStringLiteral("ok")).toBool());
    QCOMPARE(diff.value(QStringLiteral("revision")).toString(), published);
    // A request still has to name a revision.
    QCOMPARE(fenced.file(request({}, QStringLiteral("README.md")))
                 .value(QStringLiteral("error_code"))
                 .toString(),
             QStringLiteral("stale_revision"));
}

// File-service checks need only QCoreApplication, even with transitive UI libraries.
QTEST_GUILESS_MAIN(DaemonWorkspaceFilesTest)
#include "DaemonWorkspaceFilesTest.moc"
