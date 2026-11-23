#include "git/GitParser.h"
#include "TestSymlink.h"
#include "git/GitRevisionContent.h"
#include "git/GitService.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

#include <initializer_list>
#include <memory>

namespace {

QByteArray nulSeparated(const std::initializer_list<QByteArray>& records) {
    QByteArray output;
    for (const auto& record : records) {
        output.append(record);
        output.append('\0');
    }
    return output;
}

bool runGit(const QString& git, const QString& workingDirectory, const QStringList& arguments) {
    QProcess process;
    process.setProgram(git);
    process.setArguments(arguments);
    process.setWorkingDirectory(workingDirectory);
    process.start();
    return process.waitForStarted() && process.waitForFinished() && process.exitCode() == 0;
}

} // namespace

class GitParserTest final : public QObject {
    Q_OBJECT

  private slots:
    void parsesPorcelainV2Status();
    void stagesLiteralPathsAndCommitsOnlyTheIndex();
    void parsesWorktreeList();
    void discoversWorktreesAndProducesDiffs();
    void diffsSymlinksAsLinks();
    void diffsUntrackedFilesWithDecomposedNames();
    void stopsPendingCommandsBeforeDestruction();
    void answersOnlyTheNewestDiffRequest();
    void refreshesWhenGitRunsOutsideKetPlus();
    void diffsFilesBeforeTheFirstCommit();
    void stagesEditedRenameWithoutStagingRecreatedSource();
    void separatesStagedDeletionFromUntrackedReplacement();
    void fencesMutationsToTheSelectedRepository();
    void rejectsMutationWhenNestedRepositoryDisappears();
    void rejectsConfiguredWorktreeOutsideWorkspace();
    void clearsPreviousWorktreesBeforeDiscoveringNewRoot();
    void reportsGitDiscoveryFailure();
    void diffsDirtySubmoduleAsAGitlink();
    void previewReadsOnlyTheLatestRegularWorkingFile();
};

void GitParserTest::parsesPorcelainV2Status() {
    const QByteArray output = nulSeparated({
        "# branch.oid 0123456789abcdef",
        "# branch.head feature/git",
        "# branch.upstream origin/feature/git",
        "# branch.ab +2 -1",
        "1 .M N... 100644 100644 100644 aaaaa bbbbb src/main.cpp",
        "1 M. N... 100644 100644 100644 aaaaa bbbbb staged file.txt",
        "2 R. N... 100644 100644 100644 aaaaa bbbbb R100 new name.txt",
        "old name.txt",
        "? notes new.txt",
    });

    const auto snapshot = ketplus::git_parser::parseStatus(output, QStringLiteral("/repo"));
    QCOMPARE(snapshot.repositoryRoot,
             QDir::cleanPath(QFileInfo(QStringLiteral("/repo")).absoluteFilePath()));
    QCOMPARE(snapshot.head, QStringLiteral("0123456789abcdef"));
    QCOMPARE(snapshot.branch, QStringLiteral("feature/git"));
    QCOMPARE(snapshot.upstream, QStringLiteral("origin/feature/git"));
    QCOMPARE(snapshot.ahead, 2);
    QCOMPARE(snapshot.behind, 1);
    QCOMPARE(snapshot.files.size(), 4);
    QCOMPARE(snapshot.files.at(0).path, QStringLiteral("src/main.cpp"));
    QCOMPARE(snapshot.files.at(0).worktreeCode, QLatin1Char('M'));
    QVERIFY(!snapshot.files.at(0).hasStagedChange());
    QVERIFY(snapshot.files.at(0).hasUnstagedChange());
    QCOMPARE(snapshot.files.at(1).path, QStringLiteral("staged file.txt"));
    QCOMPARE(snapshot.files.at(1).indexCode, QLatin1Char('M'));
    QVERIFY(snapshot.files.at(1).hasStagedChange());
    QVERIFY(!snapshot.files.at(1).hasUnstagedChange());
    QCOMPARE(snapshot.files.at(2).path, QStringLiteral("new name.txt"));
    QCOMPARE(snapshot.files.at(2).originalPath, QStringLiteral("old name.txt"));
    QVERIFY(snapshot.files.at(3).untracked);
    QCOMPARE(snapshot.files.at(3).path, QStringLiteral("notes new.txt"));
    QVERIFY(!snapshot.files.at(3).hasStagedChange());
    QVERIFY(snapshot.files.at(3).hasUnstagedChange());
}

void GitParserTest::parsesWorktreeList() {
    const QByteArray output = nulSeparated({
        "worktree /repo",
        "HEAD 01234567",
        "branch refs/heads/main",
        "",
        "worktree /repo-feature",
        "HEAD abcdef01",
        "branch refs/heads/feature/git",
        "locked editing",
        "",
        "worktree /repo-detached",
        "HEAD fedcba98",
        "detached",
        "",
        "worktree /repo-locked",
        "HEAD 76543210",
        "branch refs/heads/locked",
        "locked",
        "",
    });

    const auto worktrees =
        ketplus::git_parser::parseWorktrees(output, QDir::cleanPath(QStringLiteral("/repo")));
    QCOMPARE(worktrees.size(), 4);
    QVERIFY(worktrees.at(0).current);
    QCOMPARE(worktrees.at(0).branch, QStringLiteral("main"));
    QCOMPARE(worktrees.at(1).branch, QStringLiteral("feature/git"));
    QVERIFY(worktrees.at(1).locked);
    QCOMPARE(worktrees.at(1).lockReason, QStringLiteral("editing"));
    QVERIFY(!worktrees.at(0).locked);
    QVERIFY(worktrees.at(2).detached);
    QCOMPARE(worktrees.at(2).head, QStringLiteral("fedcba98"));
    QVERIFY(worktrees.at(3).locked);
    QVERIFY(worktrees.at(3).lockReason.isEmpty());
}

void GitParserTest::discoversWorktreesAndProducesDiffs() {
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty()) {
        QSKIP("Git is not installed on this machine");
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("main"));
    // Git prints checkout and metadata paths verbatim. A POSIX checkout can
    // end in whitespace, including a line feed.
#ifdef Q_OS_WIN
    const QString linkedRoot = temporary.filePath(QStringLiteral("feature branch"));
#else
    const QString linkedRoot = temporary.filePath(QStringLiteral("feature branch \n "));
#endif
    QVERIFY(QDir().mkpath(root));
    QVERIFY(runGit(git, root, {QStringLiteral("init")}));

    QFile tracked(QDir(root).filePath(QStringLiteral("tracked.txt")));
    QVERIFY(tracked.open(QIODevice::WriteOnly));
    QCOMPARE(tracked.write("before\n"), 7);
    tracked.close();
    QVERIFY(runGit(git, root, {QStringLiteral("add"), QStringLiteral("tracked.txt")}));
    QVERIFY(runGit(git, root,
                   {QStringLiteral("-c"), QStringLiteral("user.name=KetPlus Test"),
                    QStringLiteral("-c"), QStringLiteral("user.email=test@ketplus.invalid"),
                    QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("Initial")}));
    QVERIFY(runGit(git, root, {QStringLiteral("branch"), QStringLiteral("feature/worktree")}));
    QVERIFY(runGit(git, root,
                   {QStringLiteral("worktree"), QStringLiteral("add"), linkedRoot,
                    QStringLiteral("feature/worktree")}));

    ketplus::GitService service;
    service.setWorkspacePath(root);
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().isRepository(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(service.worktrees().size(), 2, 5000);
    QCOMPARE(service.snapshot().repositoryRoot, QFileInfo(root).canonicalFilePath());
    QVERIFY(service.worktrees().at(0).current);
    QCOMPARE(service.worktrees().at(1).branch, QStringLiteral("feature/worktree"));

    QVERIFY(tracked.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(tracked.write("after\n"), 6);
    tracked.close();
    service.refresh();
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().files.size(), 1, 5000);

    QString trackedDiff;
    QString trackedError;
    bool trackedFinished = false;
    connect(&service, &ketplus::GitService::diffReady, &service,
            [&](const QString& path, const ketplus::GitDiffMode mode, const QString& diff,
                const QString& error) {
                if (path == tracked.fileName() && mode == ketplus::GitDiffMode::Combined) {
                    trackedDiff = diff;
                    trackedError = error;
                    trackedFinished = true;
                }
            });
    service.requestDiff(tracked.fileName());
    QTRY_VERIFY_WITH_TIMEOUT(trackedFinished, 5000);
    QVERIFY2(trackedError.isEmpty(), qPrintable(trackedError));
    QVERIFY(trackedDiff.contains(QStringLiteral("-before")));
    QVERIFY(trackedDiff.contains(QStringLiteral("+after")));

    QVERIFY(runGit(git, root, {QStringLiteral("add"), QStringLiteral("tracked.txt")}));
    QVERIFY(tracked.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(tracked.write("working\n"), 8);
    tracked.close();
    service.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().files.at(0).hasStagedChange(), 5000);
    QVERIFY(service.snapshot().files.at(0).hasUnstagedChange());

    QString stagedDiff;
    QString unstagedDiff;
    bool stagedFinished = false;
    bool unstagedFinished = false;
    connect(&service, &ketplus::GitService::diffReady, &service,
            [&](const QString& path, const ketplus::GitDiffMode mode, const QString& diff,
                const QString&) {
                if (path != tracked.fileName()) {
                    return;
                }
                if (mode == ketplus::GitDiffMode::Staged) {
                    stagedDiff = diff;
                    stagedFinished = true;
                } else if (mode == ketplus::GitDiffMode::Unstaged) {
                    unstagedDiff = diff;
                    unstagedFinished = true;
                }
            });
    // Only the newest request is answered, so ask one at a time.
    service.requestDiff(tracked.fileName(), ketplus::GitDiffMode::Staged);
    QTRY_VERIFY_WITH_TIMEOUT(stagedFinished, 5000);
    service.requestDiff(tracked.fileName(), ketplus::GitDiffMode::Unstaged);
    QTRY_VERIFY_WITH_TIMEOUT(unstagedFinished, 5000);
    QVERIFY(stagedDiff.contains(QStringLiteral("-before")));
    QVERIFY(stagedDiff.contains(QStringLiteral("+after")));
    QVERIFY(unstagedDiff.contains(QStringLiteral("-after")));
    QVERIFY(unstagedDiff.contains(QStringLiteral("+working")));

    const QString untrackedPath = QDir(root).filePath(QStringLiteral("untracked.txt"));
    QFile untracked(untrackedPath);
    QVERIFY(untracked.open(QIODevice::WriteOnly));
    QCOMPARE(untracked.write("new\n"), 4);
    untracked.close();
    service.refresh();
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().files.size(), 2, 5000);

    QString untrackedDiff;
    QString untrackedError;
    bool untrackedFinished = false;
    connect(&service, &ketplus::GitService::diffReady, &service,
            [&](const QString& path, const ketplus::GitDiffMode mode, const QString& diff,
                const QString& error) {
                if (path == untrackedPath && mode == ketplus::GitDiffMode::Unstaged) {
                    untrackedDiff = diff;
                    untrackedError = error;
                    untrackedFinished = true;
                }
            });
    service.requestDiff(untrackedPath, ketplus::GitDiffMode::Unstaged);
    QTRY_VERIFY_WITH_TIMEOUT(untrackedFinished, 5000);
    QVERIFY2(untrackedError.isEmpty(), qPrintable(untrackedError));
    QVERIFY2(untrackedDiff.contains(QStringLiteral("+new")), qPrintable(untrackedDiff));

    QVERIFY(QFileInfo(QDir(linkedRoot).filePath(".git")).isFile());
    QVERIFY(runGit(git, linkedRoot, {"switch", "--detach"}));
    service.setWorkspacePath(linkedRoot);
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().isRepository(), 5000);
    QCOMPARE(service.snapshot().repositoryRoot, QFileInfo(linkedRoot).canonicalFilePath());
    QVERIFY(service.snapshot().branch.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(service.worktrees().size(), 2, 5000);
    QVERIFY(service.worktrees().at(1).current);
    QVERIFY(service.worktrees().at(1).detached);
}

void GitParserTest::diffsSymlinksAsLinks() {
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty())
        QSKIP("Git is not installed.");

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("repo"));
    const QString outside = temporary.filePath(QStringLiteral("outside.txt"));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QStringLiteral("sub"))));
    QVERIFY(runGit(git, root, {QStringLiteral("init"), QStringLiteral("-q")}));
    const auto write = [](const QString& path, const QByteArray& contents) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
    };
    QVERIFY(write(QDir(root).filePath(QStringLiteral("sub/target.txt")), "target before\n"));
    QVERIFY(write(QDir(root).filePath(QStringLiteral("other.txt")), "other\n"));
    QVERIFY(write(outside, "outside\n"));
    const QString link = QDir(root).filePath(QStringLiteral("link.txt"));
    const QString outsideLink = QDir(root).filePath(QStringLiteral("outside-link.txt"));
    const QString firstLinkError =
        ketplus::test::createSymlink(QStringLiteral("sub/target.txt"), link);
    QVERIFY2(firstLinkError.isEmpty(), qPrintable(firstLinkError));
    const QString outsideLinkError = ketplus::test::createSymlink(outside, outsideLink);
    QVERIFY2(outsideLinkError.isEmpty(), qPrintable(outsideLinkError));
    QVERIFY(runGit(
        git, root,
        {QStringLiteral("config"), QStringLiteral("core.symlinks"), QStringLiteral("true")}));
    QVERIFY(runGit(git, root, {QStringLiteral("add"), QStringLiteral(".")}));
    QVERIFY(runGit(git, root,
                   {QStringLiteral("-c"), QStringLiteral("user.name=KetPlus Test"),
                    QStringLiteral("-c"), QStringLiteral("user.email=test@ketplus.invalid"),
                    QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"),
                    QStringLiteral("Initial")}));

    // Retarget both links and change the old target, so a diff of the target would differ.
    QVERIFY(QFile::remove(link));
    const QString retargetError = ketplus::test::createSymlink(QStringLiteral("other.txt"), link);
    QVERIFY2(retargetError.isEmpty(), qPrintable(retargetError));
    QVERIFY(QFile::remove(outsideLink));
    const QString outsideRetargetError =
        ketplus::test::createSymlink(QStringLiteral("other.txt"), outsideLink);
    QVERIFY2(outsideRetargetError.isEmpty(), qPrintable(outsideRetargetError));
    QVERIFY(write(QDir(root).filePath(QStringLiteral("sub/target.txt")), "target after\n"));

    ketplus::GitService service;
    service.setWorkspacePath(root);
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().files.size(), 3, 5000);

    QHash<QString, QString> diffs;
    QHash<QString, QString> errors;
    connect(&service, &ketplus::GitService::diffReady, &service,
            [&](const QString& path, const ketplus::GitDiffMode, const QString& diff,
                const QString& error) {
                diffs.insert(path, diff);
                errors.insert(path, error);
            });
    // Only the newest request is answered, so ask one at a time.
    for (const QString& path : {link, outsideLink}) {
        service.requestDiff(path);
        QTRY_VERIFY_WITH_TIMEOUT(diffs.contains(path), 5000);
    }

    QVERIFY2(errors.value(link).isEmpty(), qPrintable(errors.value(link)));
    QVERIFY2(diffs.value(link).contains(QStringLiteral("-sub/target.txt")),
             qPrintable(diffs.value(link)));
    QVERIFY(diffs.value(link).contains(QStringLiteral("+other.txt")));
    QVERIFY(!diffs.value(link).contains(QStringLiteral("target after")));

    QVERIFY2(errors.value(outsideLink).isEmpty(), qPrintable(errors.value(outsideLink)));
    QVERIFY2(diffs.value(outsideLink).contains(QStringLiteral("+other.txt")),
             qPrintable(diffs.value(outsideLink)));
}

void GitParserTest::diffsUntrackedFilesWithDecomposedNames() {
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty())
        QSKIP("Git is not installed.");

    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QString root = QFileInfo(workspace.path()).canonicalFilePath();
    QVERIFY(runGit(git, root, {QStringLiteral("init"), QStringLiteral("-q")}));
    // Some tools write names decomposed (NFD); Git on macOS reports them precomposed (NFC).
    const QString name =
        QStringLiteral("t\u00e0i-li\u1ec7u.md").normalized(QString::NormalizationForm_D);
    const QString path = QDir(root).filePath(name);
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("xin chao\n"), 9);
    }

    ketplus::GitService service;
    service.setWorkspacePath(root);
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().files.size(), 1, 5000);
    QVERIFY(service.snapshot().files.at(0).untracked);

    QString diff;
    QString error;
    bool finished = false;
    connect(&service, &ketplus::GitService::diffReady, &service,
            [&](const QString&, const ketplus::GitDiffMode, const QString& output,
                const QString& message) {
                diff = output;
                error = message;
                finished = true;
            });
    service.requestDiff(path, ketplus::GitDiffMode::Unstaged);
    QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(diff.contains(QStringLiteral("+xin chao")), qPrintable(diff));
}

void GitParserTest::stopsPendingCommandsBeforeDestruction() {
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty())
        QSKIP("Git is not installed.");
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    QVERIFY(runGit(git, workspace.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    auto service = std::make_unique<ketplus::GitService>();
    service->setWorkspacePath(workspace.path());
    QTRY_VERIFY_WITH_TIMEOUT(!service->worktrees().isEmpty(), 5000);
    service->refresh();
    QList<QPointer<QProcess>> pending;
    for (auto* process : service->findChildren<QProcess*>()) {
        if (process->state() != QProcess::NotRunning) {
            QVERIFY(process->waitForStarted(5000));
            pending.append(process);
        }
    }
    QVERIFY(!pending.isEmpty());
    bool runningAtOwnerDestruction = false;
    connect(service.get(), &QObject::destroyed, this, [&runningAtOwnerDestruction, pending] {
        for (const auto& process : pending)
            runningAtOwnerDestruction |= process && process->state() != QProcess::NotRunning;
    });
    service.reset();
    // Producers must stop while the derived service's snapshot/worktree data still exists.
    QVERIFY(!runningAtOwnerDestruction);
    for (const auto& process : pending)
        QVERIFY(process.isNull());
}

void GitParserTest::diffsFilesBeforeTheFirstCommit() {
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty())
        QSKIP("Git is not installed.");

    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    // Canonical, because a deleted file cannot be resolved through /var -> /private/var.
    const QString root = QFileInfo(workspace.path()).canonicalFilePath();
    QVERIFY(runGit(git, root, {QStringLiteral("init"), QStringLiteral("-q")}));
    const QString staged = QDir(root).filePath(QStringLiteral("staged.txt"));
    const QString removed = QDir(root).filePath(QStringLiteral("removed.txt"));
    for (const QString& path : {staged, removed}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("first\n"), 6);
    }
    // A tracked symlink whose target does not exist is still an entry on disk.
    const QString danglingLink = QDir(root).filePath(QStringLiteral("dangling-link"));
    const QString danglingError =
        ketplus::test::createSymlink(QStringLiteral("missing-target"), danglingLink);
    QVERIFY2(danglingError.isEmpty(), qPrintable(danglingError));
    QVERIFY(runGit(
        git, root,
        {QStringLiteral("config"), QStringLiteral("core.symlinks"), QStringLiteral("true")}));
    QVERIFY(runGit(git, root, {QStringLiteral("add"), QStringLiteral(".")}));
    // Staged, then deleted from the working tree: only the index still has it.
    QVERIFY(QFile::remove(removed));

    ketplus::GitService service;
    service.setWorkspacePath(root);
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().files.size(), 3, 5000);
    QCOMPARE(service.snapshot().head, QStringLiteral("(initial)"));

    QHash<QString, QString> diffs;
    QHash<QString, QString> errors;
    connect(&service, &ketplus::GitService::diffReady, &service,
            [&](const QString& path, const ketplus::GitDiffMode, const QString& diff,
                const QString& error) {
                diffs.insert(path, diff);
                errors.insert(path, error);
            });
    for (const QString& path : {staged, removed, danglingLink}) {
        service.requestDiff(path);
        QTRY_VERIFY_WITH_TIMEOUT(diffs.contains(path), 5000);
    }
    QVERIFY2(errors.value(danglingLink).isEmpty(), qPrintable(errors.value(danglingLink)));
    QVERIFY2(diffs.value(danglingLink).contains(QStringLiteral("new file mode 120000")),
             qPrintable(diffs.value(danglingLink)));
    QVERIFY(diffs.value(danglingLink).contains(QStringLiteral("+missing-target")));
    QVERIFY2(errors.value(staged).isEmpty(), qPrintable(errors.value(staged)));
    QVERIFY2(diffs.value(staged).contains(QStringLiteral("+first")),
             qPrintable(diffs.value(staged)));
    // Absent from both the working tree and the (empty) HEAD: no combined change.
    QVERIFY2(errors.value(removed).isEmpty(), qPrintable(errors.value(removed)));
    QVERIFY2(diffs.contains(removed) && diffs.value(removed).isEmpty(),
             qPrintable(diffs.value(removed)));
}

void GitParserTest::answersOnlyTheNewestDiffRequest() {
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty())
        QSKIP("Git is not installed.");

    QTemporaryDir workspace;
    QTemporaryDir other;
    QVERIFY(workspace.isValid() && other.isValid());
    const QString root = QFileInfo(workspace.path()).canonicalFilePath();
    QVERIFY(runGit(git, root, {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(runGit(git, other.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    const QString first = QDir(root).filePath(QStringLiteral("first.txt"));
    const QString second = QDir(root).filePath(QStringLiteral("second.txt"));
    for (const QString& path : {first, second}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("line\n"), 5);
    }

    ketplus::GitService service;
    service.setWorkspacePath(root);
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().files.size(), 2, 5000);

    QStringList answered;
    connect(&service, &ketplus::GitService::diffReady, &service,
            [&](const QString& path, const ketplus::GitDiffMode, const QString&, const QString&) {
                answered.append(path);
            });
    // An older reply arriving later would replace the diff the user asked for last.
    service.requestDiff(first, ketplus::GitDiffMode::Unstaged);
    service.requestDiff(second, ketplus::GitDiffMode::Unstaged);
    QTRY_COMPARE_WITH_TIMEOUT(answered.size(), 1, 5000);
    QTest::qWait(300);
    QCOMPARE(answered, QStringList{second});

    // A diff of the previous workspace never opens in the new one.
    answered.clear();
    service.requestDiff(first, ketplus::GitDiffMode::Unstaged);
    service.setWorkspacePath(other.path());
    QTest::qWait(500);
    QVERIFY(answered.isEmpty());
}

void GitParserTest::refreshesWhenGitRunsOutsideKetPlus() {
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty())
        QSKIP("Git is not installed.");

    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QString root = QFileInfo(workspace.path()).canonicalFilePath();
    QVERIFY(runGit(git, root, {QStringLiteral("init"), QStringLiteral("-q")}));
    {
        QFile file(QDir(root).filePath(QStringLiteral("notes.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("notes\n"), 6);
    }

    ketplus::GitService service;
    service.setWorkspacePath(root);
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().files.size(), 1, 5000);
    QVERIFY(service.snapshot().files.at(0).untracked);

    // Like a terminal or an agent: Git runs without KetPlus asking for a refresh.
    QVERIFY(runGit(git, root, {QStringLiteral("add"), QStringLiteral("notes.txt")}));
    QTRY_VERIFY_WITH_TIMEOUT(!service.snapshot().files.isEmpty() &&
                                 service.snapshot().files.at(0).hasStagedChange(),
                             5000);

    QVERIFY(runGit(git, root,
                   {QStringLiteral("-c"), QStringLiteral("user.name=KetPlus Test"),
                    QStringLiteral("-c"), QStringLiteral("user.email=test@ketplus.invalid"),
                    QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"),
                    QStringLiteral("Initial")}));
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().files.isEmpty(), 5000);
    QVERIFY(service.snapshot().head != QStringLiteral("(initial)"));

    QVERIFY(runGit(git, root,
                   {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("-b"),
                    QStringLiteral("feature/watch")}));
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().branch, QStringLiteral("feature/watch"), 5000);
}

QTEST_MAIN(GitParserTest)
#include "GitParserTest.moc"

void GitParserTest::stagesLiteralPathsAndCommitsOnlyTheIndex() {
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty())
        QSKIP("git unavailable");
    QTemporaryDir repo;
    QVERIFY(repo.isValid());
    QVERIFY(runGit(git, repo.path(), {"init", "-q"}));
    QVERIFY(runGit(git, repo.path(), {"config", "user.name", "KetPlus Test"}));
    QVERIFY(runGit(git, repo.path(), {"config", "user.email", "test@ketplus.invalid"}));
    QVERIFY(runGit(git, repo.path(), {"config", "commit.gpgsign", "false"}));
    QVERIFY(QDir().mkpath(repo.filePath("empty-hooks")));
    QVERIFY(runGit(git, repo.path(), {"config", "core.hooksPath", repo.filePath("empty-hooks")}));
    for (const QString name : {QStringLiteral("a[1].txt"), QStringLiteral("kept.txt")}) {
        QFile file(repo.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("content\n");
    }
    ketplus::GitService service;
    QSignalSpy finished(&service, &ketplus::GitService::mutationFinished);
    service.setWorkspacePath(repo.path());
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().files.size(), 2, 5000);
    service.stageFile(repo.filePath("../outside.txt"));
    QCOMPARE(finished.size(), 1);
    QVERIFY(!finished.takeFirst().at(1).toBool());
    service.stageFile(repo.filePath("a[1].txt"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
    finished.removeFirst();
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().files.first().hasStagedChange(), 5000);
    service.unstageFile(repo.filePath("a[1].txt"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
    finished.removeFirst();
    QVERIFY(QFile::exists(repo.filePath("a[1].txt")));
    QTRY_VERIFY_WITH_TIMEOUT(!service.snapshot().files.first().hasStagedChange(), 5000);
    service.stageFile(repo.filePath("a[1].txt"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
    finished.removeFirst();
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().files.first().hasStagedChange(), 5000);
    service.commitStaged("Commit only the selected file");
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot().files.size(), 1, 5000);
    QCOMPARE(service.snapshot().files.first().path, QStringLiteral("kept.txt"));
    QVERIFY(service.snapshot().files.first().untracked);
}

namespace {

bool writeGitFixture(const QString& path, const QByteArray& content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

bool commitGitFixture(const QString& git, const QDir& root) {
    return runGit(git, root.path(),
                  {"-c", "user.name=KetPlus Test", "-c", "user.email=test@ketplus.invalid", "-c",
                   "commit.gpgsign=false", "commit", "-qm", "Initial"});
}

} // namespace

void GitParserTest::stagesEditedRenameWithoutStagingRecreatedSource() {
    const QString git = QStandardPaths::findExecutable("git");
    if (git.isEmpty())
        QSKIP("Git is not installed.");
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir root(QFileInfo(temp.path()).canonicalFilePath());
    const QString oldName = QString::fromUtf8("cũ file.txt");
    const QString newName = QString::fromUtf8("mới file.txt");
    const QString oldPath = root.filePath(oldName);
    const QString newPath = root.filePath(newName);
    QVERIFY(runGit(git, root.path(), {"init", "-q"}));
    QVERIFY(runGit(git, root.path(), {"config", "core.precomposeUnicode", "true"}));
    QVERIFY(writeGitFixture(oldPath, "one\ntwo\nthree\nfour\nfive\n"));
    QVERIFY(runGit(git, root.path(), {"add", "--", oldName}));
    QVERIFY(commitGitFixture(git, root));
    QVERIFY(runGit(git, root.path(), {"mv", "--", oldName, newName}));
    QVERIFY(writeGitFixture(newPath, "one\ntwo\nthree\nfour\nfive\nnew line\n"));

    ketplus::GitService service;
    QSignalSpy diff(&service, &ketplus::GitService::diffReady);
    QSignalSpy mutations(&service, &ketplus::GitService::mutationFinished);
    service.setWorkspacePath(root.path());
    QTRY_COMPARE(service.snapshot().files.size(), 1);
    QCOMPARE(service.snapshot().files.first().originalPath, oldName);
    QCOMPARE(service.snapshot().files.first().path, newName);
    service.requestDiff(newPath, ketplus::GitDiffMode::Staged);
    QTRY_COMPARE(diff.size(), 1);
    QVERIFY(diff.first().at(2).toString().contains("similarity index 100%"));
    QVERIFY(diff.first().at(3).toString().isEmpty());

    service.stageFile(newPath);
    QTRY_COMPARE(mutations.size(), 1);
    auto result = mutations.takeFirst();
    QVERIFY2(result.at(1).toBool(), qPrintable(result.at(2).toString()));
    QTRY_VERIFY(!service.snapshot().files.first().hasUnstagedChange());
    QVERIFY(writeGitFixture(newPath, "one\ntwo\nthree\nfour\nfive\nnew line\nnext edit\n"));
    QVERIFY(writeGitFixture(oldPath, "untracked replacement\n"));
    service.refresh();
    QTRY_COMPARE(service.snapshot().files.size(), 2);
    service.stageFile(newPath);
    QTRY_COMPARE(mutations.size(), 1);
    result = mutations.takeFirst();
    QVERIFY2(result.at(1).toBool(), qPrintable(result.at(2).toString()));
    QTRY_VERIFY(service.snapshot().files.last().untracked);
    QCOMPARE(service.snapshot().files.last().path, oldName);
    QTRY_VERIFY(!service.snapshot().files.first().hasUnstagedChange());

    service.unstageFile(newPath);
    QTRY_COMPARE(mutations.size(), 1);
    result = mutations.takeFirst();
    QVERIFY2(result.at(1).toBool(), qPrintable(result.at(2).toString()));
    QTRY_VERIFY(!service.snapshot().files.first().hasStagedChange());
    QFile oldFile(oldPath);
    QVERIFY(oldFile.open(QIODevice::ReadOnly));
    QCOMPARE(oldFile.readAll(), QByteArray("untracked replacement\n"));
}

void GitParserTest::separatesStagedDeletionFromUntrackedReplacement() {
    const QString git = QStandardPaths::findExecutable("git");
    if (git.isEmpty())
        QSKIP("Git is not installed.");
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir root(temp.path());
    const QString name = QStringLiteral("replacement file.txt");
    const QString path = root.filePath(name);
    QVERIFY(runGit(git, root.path(), {"init", "-q"}));
    QVERIFY(writeGitFixture(path, "original\n"));
    QVERIFY(runGit(git, root.path(), {"add", "--", name}));
    QVERIFY(commitGitFixture(git, root));
    QVERIFY(runGit(git, root.path(), {"rm", "--", name}));
    QVERIFY(writeGitFixture(path, "replacement\n"));

    ketplus::GitService service;
    QSignalSpy diff(&service, &ketplus::GitService::diffReady);
    service.setWorkspacePath(root.path());
    QTRY_COMPARE(service.snapshot().files.size(), 2);
    service.requestDiff(path, ketplus::GitDiffMode::Staged);
    QTRY_COMPARE(diff.size(), 1);
    auto result = diff.takeFirst();
    QVERIFY(result.at(3).toString().isEmpty());
    QVERIFY(result.at(2).toString().contains("-original"));
    QVERIFY(!result.at(2).toString().contains("+replacement"));

    service.requestDiff(path, ketplus::GitDiffMode::Unstaged);
    QTRY_COMPARE(diff.size(), 1);
    result = diff.takeFirst();
    QVERIFY(result.at(3).toString().isEmpty());
    QVERIFY(result.at(2).toString().contains("+replacement"));
    QVERIFY(!result.at(2).toString().contains("-original"));
}

void GitParserTest::fencesMutationsToTheSelectedRepository() {
    const QString git = QStandardPaths::findExecutable("git");
    if (git.isEmpty())
        QSKIP("Git is not installed.");
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir root(temp.path());
    const QString selected = root.filePath("selected");
    const QString neighbor = root.filePath("neighbor");
    QVERIFY(QDir().mkpath(selected));
    QVERIFY(QDir().mkpath(neighbor));
    for (const auto& repo : {selected, neighbor}) {
        QVERIFY(runGit(git, repo, {"init", "-q"}));
        QVERIFY(writeGitFixture(QDir(repo).filePath("selected.txt"), "content\n"));
    }
    ketplus::GitService service;
    QSignalSpy mutations(&service, &ketplus::GitService::mutationFinished);
    service.setWorkspacePath(selected);
    QTRY_COMPARE(service.snapshot().files.size(), 1);

    const QByteArray gitDir = qgetenv("GIT_DIR");
    const QByteArray gitWorkTree = qgetenv("GIT_WORK_TREE");
    const auto restoreEnvironment = qScopeGuard([&] {
        if (gitDir.isNull())
            qunsetenv("GIT_DIR");
        else
            qputenv("GIT_DIR", gitDir);
        if (gitWorkTree.isNull())
            qunsetenv("GIT_WORK_TREE");
        else
            qputenv("GIT_WORK_TREE", gitWorkTree);
    });
    qputenv("GIT_DIR", QFile::encodeName(QDir(neighbor).filePath(".git")));
    qputenv("GIT_WORK_TREE", QFile::encodeName(neighbor));
    service.stageFile(QDir(selected).filePath("selected.txt"));
    QTRY_COMPARE(mutations.size(), 1);
    auto result = mutations.takeFirst();
    QVERIFY2(result.at(1).toBool(), qPrintable(result.at(2).toString()));
    QVERIFY(!QFile::exists(QDir(neighbor).filePath(".git/index")));
    QTRY_VERIFY(service.snapshot().files.first().hasStagedChange());

    // A reentrant workspace switch must not retarget a mutation already accepted.
    connect(&service, &ketplus::GitService::mutationRunningChanged, &service, [&](bool running) {
        if (running)
            service.setWorkspacePath(neighbor);
    });
    service.unstageFile(QDir(selected).filePath("selected.txt"));
    QTRY_COMPARE(service.snapshot().repositoryRoot, QFileInfo(neighbor).canonicalFilePath());
    QTRY_VERIFY(!service.mutationRunning());
    QCOMPARE(mutations.size(), 0);
    QVERIFY(service.snapshot().files.first().untracked);
}

void GitParserTest::rejectsMutationWhenNestedRepositoryDisappears() {
    const QString git = QStandardPaths::findExecutable("git");
    if (git.isEmpty())
        QSKIP("Git is not installed.");
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QDir parent(temp.path());
    const QDir child(parent.filePath("child"));
    QVERIFY(parent.mkdir("child"));
    QVERIFY(runGit(git, parent.path(), {"init", "-q"}));
    QVERIFY(runGit(git, child.path(), {"init", "-q"}));
    const QString file = child.filePath("changed.txt");
    QVERIFY(writeGitFixture(file, "nested worktree change\n"));
    ketplus::GitService service;
    QSignalSpy mutations(&service, &ketplus::GitService::mutationFinished);
    service.setWorkspacePath(child.path());
    QTRY_COMPARE(service.snapshot().files.size(), 1);
    QVERIFY(parent.rename("child/.git", "moved-git"));

    service.stageFile(file);
    QTRY_COMPARE(mutations.size(), 1);
    QVERIFY(!mutations.first().at(1).toBool());
    QVERIFY(!mutations.first().at(2).toString().isEmpty());
    QVERIFY(!QFile::exists(parent.filePath(".git/index")));
}

void GitParserTest::rejectsConfiguredWorktreeOutsideWorkspace() {
    const QString git = QStandardPaths::findExecutable("git");
    if (git.isEmpty())
        QSKIP("Git is not installed.");
    QTemporaryDir selected;
    QTemporaryDir other;
    QVERIFY(selected.isValid() && other.isValid());
    QVERIFY(runGit(git, selected.path(), {"init", "-q"}));
    QVERIFY(runGit(git, other.path(), {"init", "-q"}));
    QVERIFY(runGit(git, selected.path(), {"config", "core.worktree", other.path()}));
    ketplus::GitService service;
    QSignalSpy errors(&service, &ketplus::GitService::errorOccurred);
    service.setWorkspacePath(selected.path());
    QTRY_COMPARE(errors.size(), 1);
    QVERIFY(!service.snapshot().isRepository());
    QVERIFY(service.worktrees().isEmpty());
}

void GitParserTest::clearsPreviousWorktreesBeforeDiscoveringNewRoot() {
    const QString git = QStandardPaths::findExecutable("git");
    if (git.isEmpty())
        QSKIP("Git is not installed.");
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid() && second.isValid());
    QVERIFY(runGit(git, first.path(), {"init", "-q"}));
    QVERIFY(runGit(git, second.path(), {"init", "-q"}));
    ketplus::GitService service;
    service.setWorkspacePath(first.path());
    QTRY_COMPARE(service.worktrees().size(), 1);
    QSignalSpy changed(&service, &ketplus::GitService::worktreesChanged);
    service.setWorkspacePath(second.path());
    QCOMPARE(changed.size(), 1);
    QVERIFY(changed.first().first().value<QVector<ketplus::GitWorktree>>().isEmpty());
    QTRY_COMPARE(service.worktrees().size(), 1);
    QCOMPARE(service.worktrees().first().path, QFileInfo(second.path()).canonicalFilePath());
}

void GitParserTest::reportsGitDiscoveryFailure() {
    ketplus::GitService service;
    if (!service.isAvailable())
        QSKIP("Git is not installed.");
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString absent = temp.filePath("missing");
    QSignalSpy errors(&service, &ketplus::GitService::errorOccurred);
    service.setWorkspacePath(absent);
    QTRY_COMPARE(errors.size(), 1);
    QVERIFY(!errors.first().first().toString().isEmpty());
    QVERIFY(!service.snapshot().isRepository());
}

void GitParserTest::diffsDirtySubmoduleAsAGitlink() {
    const QString git = QStandardPaths::findExecutable("git");
    if (git.isEmpty())
        QSKIP("Git is not installed.");
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir checkout(temp.filePath("checkout"));
    const QDir source(temp.filePath("module source"));
    QVERIFY(QDir().mkpath(checkout.path()));
    QVERIFY(QDir().mkpath(source.path()));
    QVERIFY(runGit(git, checkout.path(), {"init", "-q"}));
    QVERIFY(runGit(git, source.path(), {"init", "-q"}));
    QVERIFY(writeGitFixture(source.filePath("tracked.txt"), "original\n"));
    QVERIFY(runGit(git, source.path(), {"add", "--", "tracked.txt"}));
    QVERIFY(commitGitFixture(git, source));
    const QString module = QStringLiteral("module with space");
    QVERIFY(runGit(
        git, checkout.path(),
        {"-c", "protocol.file.allow=always", "submodule", "add", "--", source.path(), module}));
    QVERIFY(commitGitFixture(git, checkout));
    QVERIFY(runGit(git, checkout.path(), {"config", "diff.submodule", "diff"}));
    QVERIFY(writeGitFixture(checkout.filePath(module + "/tracked.txt"), "edited\n"));

    ketplus::GitService service;
    QSignalSpy diff(&service, &ketplus::GitService::diffReady);
    service.setWorkspacePath(checkout.path());
    QTRY_COMPARE(service.snapshot().files.size(), 1);
    const auto status = service.snapshot().files.first();
    QCOMPARE(status.path, module);
    QVERIFY(status.hasUnstagedChange());
    QVERIFY(!status.hasStagedChange());
    service.requestDiff(checkout.filePath(module), ketplus::GitDiffMode::Unstaged);
    QTRY_COMPARE(diff.size(), 1);
    QVERIFY(diff.first().at(3).toString().isEmpty());
    QVERIFY(diff.first().at(2).toString().contains("Subproject commit"));
    QVERIFY(diff.first().at(2).toString().contains("-dirty"));
}

void GitParserTest::previewReadsOnlyTheLatestRegularWorkingFile() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath("latest.md");
    QVERIFY(writeGitFixture(path, "# New working file\n"));
    ketplus::GitRevisionContent revision;
    QSignalSpy results(&revision, &ketplus::GitRevisionContent::ready);
    QSemaphore started;
    QSemaphore release;
    auto* pool = QThreadPool::globalInstance();
    const int originalThreadCount = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    pool->start([&] {
        started.release();
        release.acquire();
    });
    const auto restorePool = qScopeGuard([&] {
        release.release();
        pool->setMaxThreadCount(originalThreadCount);
        pool->waitForDone();
    });
    QVERIFY(started.tryAcquire(1, 5000));
    revision.load(temp.filePath("deleted.md"), ketplus::GitDiffMode::Unstaged);
    revision.load(path, ketplus::GitDiffMode::Unstaged);
    QCoreApplication::processEvents();
    QVERIFY(results.isEmpty()); // The UI can process events while the read is still queued.
    release.release();
    QTRY_COMPARE(results.size(), 1);
    auto result = results.takeFirst();
    QCOMPARE(result.at(0).toString(), QStringLiteral("# New working file\n"));
    QVERIFY(result.at(1).toString().isEmpty());

    revision.load(temp.path(), ketplus::GitDiffMode::Combined);
    QTRY_COMPARE(results.size(), 1);
    result = results.takeFirst();
    QVERIFY(result.at(0).toString().isEmpty());
    QVERIFY(!result.at(1).toString().isEmpty());
}
