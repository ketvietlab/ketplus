#include "git/GitParser.h"
#include "git/GitService.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <initializer_list>

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
    QCOMPARE(snapshot.repositoryRoot, QStringLiteral("/repo"));
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
    });

    const auto worktrees =
        ketplus::git_parser::parseWorktrees(output, QDir::cleanPath(QStringLiteral("/repo")));
    QCOMPARE(worktrees.size(), 3);
    QVERIFY(worktrees.at(0).current);
    QCOMPARE(worktrees.at(0).branch, QStringLiteral("main"));
    QCOMPARE(worktrees.at(1).branch, QStringLiteral("feature/git"));
    QCOMPARE(worktrees.at(1).lockReason, QStringLiteral("editing"));
    QVERIFY(worktrees.at(2).detached);
    QCOMPARE(worktrees.at(2).head, QStringLiteral("fedcba98"));
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
    QVERIFY(runGit(git, root,
                   {QStringLiteral("branch"), QStringLiteral("feature/worktree")}));
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
    QVERIFY(untrackedDiff.contains(QStringLiteral("+new")));
}

QTEST_MAIN(GitParserTest)
#include "GitParserTest.moc"
