#include "git/GitParser.h"
#include "git/GitService.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
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
    void parsesWorktreeList();
    void discoversWorktreesAndProducesDiffs();
    void diffsSymlinksAsLinks();
    void diffsUntrackedFilesWithDecomposedNames();
    void stopsPendingCommandsBeforeDestruction();
    void diffsFilesBeforeTheFirstCommit();
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
    const QString linkedRoot = temporary.filePath(QStringLiteral("feature"));
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
    service.requestDiff(tracked.fileName(), ketplus::GitDiffMode::Staged);
    service.requestDiff(tracked.fileName(), ketplus::GitDiffMode::Unstaged);
    QTRY_VERIFY_WITH_TIMEOUT(stagedFinished && unstagedFinished, 5000);
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
    QVERIFY(QFile::link(QStringLiteral("sub/target.txt"), link));
    QVERIFY(QFile::link(outside, outsideLink));
    QVERIFY(runGit(git, root, {QStringLiteral("add"), QStringLiteral(".")}));
    QVERIFY(runGit(git, root,
                   {QStringLiteral("-c"), QStringLiteral("user.name=KetPlus Test"),
                    QStringLiteral("-c"), QStringLiteral("user.email=test@ketplus.invalid"),
                    QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"),
                    QStringLiteral("Initial")}));

    // Retarget both links and change the old target, so a diff of the target would differ.
    QVERIFY(QFile::remove(link));
    QVERIFY(QFile::link(QStringLiteral("other.txt"), link));
    QVERIFY(QFile::remove(outsideLink));
    QVERIFY(QFile::link(QStringLiteral("other.txt"), outsideLink));
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
    service.requestDiff(link);
    service.requestDiff(outsideLink);
    QTRY_COMPARE_WITH_TIMEOUT(diffs.size(), 2, 5000);

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
    QVERIFY(QFile::link(QStringLiteral("missing-target"), danglingLink));
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
    service.requestDiff(staged);
    service.requestDiff(removed);
    service.requestDiff(danglingLink);
    QTRY_COMPARE_WITH_TIMEOUT(diffs.size(), 3, 5000);
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

QTEST_MAIN(GitParserTest)
#include "GitParserTest.moc"
