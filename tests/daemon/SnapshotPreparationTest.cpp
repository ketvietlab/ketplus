#include "runner/sync/SnapshotPreparation.h"

#include "snapshot/SnapshotStore.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace ketplus::daemon::runner::sync;
namespace snapshot = ketplus::runner::snapshot;

namespace {

QByteArray git(const QString& home, const QString& directory, const QStringList& arguments,
               bool* ok = nullptr) {
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("PATH"), qEnvironmentVariable("PATH"));
    environment.insert(QStringLiteral("HOME"), home);
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), home + QStringLiteral("/.config"));
    environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
    environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral("/dev/null"));
    for (const auto* name : {"GIT_AUTHOR_NAME", "GIT_COMMITTER_NAME"})
        environment.insert(QString::fromLatin1(name), QStringLiteral("Fixture"));
    for (const auto* name : {"GIT_AUTHOR_EMAIL", "GIT_COMMITTER_EMAIL"})
        environment.insert(QString::fromLatin1(name), QStringLiteral("fixture@example.invalid"));
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory);
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("-c"), QStringLiteral("init.defaultBranch=main")} +
                      arguments);
    process.waitForFinished(60'000);
    if (ok)
        *ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    return process.readAllStandardOutput().trimmed();
}

void write(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(content);
}

QString canonical(const QString& path) { return QFileInfo(path).canonicalFilePath(); }

} // namespace

class SnapshotPreparationTest : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        root_ = canonical(dir_->path());
        home_ = root_ + QStringLiteral("/home");
        repo_ = root_ + QStringLiteral("/repo");
        stores_ = root_ + QStringLiteral("/stores");
        QVERIFY(QDir().mkpath(home_));
        QVERIFY(QDir().mkpath(repo_));
        bool ok = false;
        git(home_, repo_, {QStringLiteral("init"), QStringLiteral("-q")}, &ok);
        QVERIFY(ok);
        write(repo_ + QStringLiteral("/src/main.c"), "int main() { return 0; }\n");
        git(home_, repo_, {QStringLiteral("add"), QStringLiteral(".")}, &ok);
        QVERIFY(ok);
        git(home_, repo_, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("one")},
            &ok);
        QVERIFY(ok);
        head_ = QString::fromLatin1(
            git(home_, repo_, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}));
    }

    void cleanup() { dir_.reset(); }

    void findsTheWorktreeOfACwd() {
        auto found = worktreeOf(repo_ + QStringLiteral("/src"));
        QVERIFY(found);
        QCOMPARE(found->root, repo_);
        QCOMPARE(found->relative, QStringLiteral("src"));
        found = worktreeOf(repo_);
        QVERIFY(found);
        QCOMPARE(found->relative, QString{});
        QVERIFY(!worktreeOf(repo_ + QStringLiteral("/.git")));
        QVERIFY(!worktreeOf(repo_ + QStringLiteral("/.git/refs")));
        QVERIFY(!worktreeOf(root_));
        QVERIFY(!worktreeOf(repo_ + QStringLiteral("/missing")));
    }

    void aNestedRepositoryIsItsOwnWorktree() {
        const QString inner = repo_ + QStringLiteral("/vendor/lib");
        QVERIFY(QDir().mkpath(inner));
        bool ok = false;
        git(home_, inner, {QStringLiteral("init"), QStringLiteral("-q")}, &ok);
        QVERIFY(ok);
        const auto found = worktreeOf(inner);
        QVERIFY(found);
        QCOMPARE(found->root, inner);
    }

    void preparesASnapshotAndHoldsItInTheStore() {
        write(repo_ + QStringLiteral("/src/new.c"), "int added;\n");
        const auto outcome = prepareSnapshot(request(repo_ + QStringLiteral("/src")));
        QVERIFY2(outcome.prepared, qPrintable(outcome.code));
        const Prepared& prepared = *outcome.prepared;
        QCOMPARE(prepared.worktreeRoot, repo_);
        QCOMPARE(prepared.cwd, QStringLiteral("src"));
        QCOMPARE(prepared.worktreeId, repo_);
        QCOMPARE(prepared.repositoryId, canonical(repo_ + QStringLiteral("/.git")));
        QCOMPARE(prepared.sourceHead, head_);
        QVERIFY(!prepared.sourceHeadUnreadable);
        QCOMPARE(prepared.objectFormat, QStringLiteral("sha1"));
        QVERIFY(prepared.warnings.isEmpty());
        QVERIFY(snapshot::isObjectId(prepared.snapshotId));
        QVERIFY(prepared.snapshotId != head_);
        QCOMPARE(prepared.storePath, storePathFor(stores_, prepared.repositoryId));
        QVERIFY(prepared.storePath.startsWith(stores_ + QLatin1Char('/')));
        QVERIFY(!prepared.storePath.contains(QStringLiteral("repo")));

        const auto refs = snapshot::listRefs(prepared.storePath);
        QVERIFY(refs);
        QCOMPARE(refs.value().value(QStringLiteral("inflight/job-1")), prepared.snapshotId);
    }

    void reportsWarningsWithoutPaths() {
        write(repo_ + QStringLiteral("/.gitattributes"), "*.bin filter=lfs\n");
        write(repo_ + QStringLiteral("/data.bin"), "x");
        const auto outcome = prepareSnapshot(request(repo_));
        QVERIFY2(outcome.prepared, qPrintable(outcome.code));
        QCOMPARE(outcome.prepared->warnings, QStringList{QStringLiteral("lfs_not_synced")});
        QCOMPARE(outcome.prepared->lfsPaths, QStringList{QStringLiteral("data.bin")});
    }

    void anUnbornBranchHasNoSourceHead() {
        const QString fresh = root_ + QStringLiteral("/fresh");
        QVERIFY(QDir().mkpath(fresh));
        bool ok = false;
        git(home_, fresh, {QStringLiteral("init"), QStringLiteral("-q")}, &ok);
        QVERIFY(ok);
        write(fresh + QStringLiteral("/a.txt"), "a\n");
        const auto outcome = prepareSnapshot(request(fresh));
        QVERIFY2(outcome.prepared, qPrintable(outcome.code));
        QVERIFY(outcome.prepared->sourceHead.isEmpty());
        QVERIFY(!outcome.prepared->sourceHeadUnreadable);
    }

    void refusesWhatIsNotAWorktree() {
        QCOMPARE(prepareSnapshot(request(root_)).code, prepare::notAWorktree);
        QCOMPARE(prepareSnapshot(request(repo_ + QStringLiteral("/.git"))).code,
                 prepare::notAWorktree);
        QCOMPARE(prepareSnapshot(request(repo_ + QStringLiteral("/missing"))).code,
                 prepare::notAWorktree);
    }

    void refusesAMissingGit() {
        snapshot::GitTool tool;
        tool.program = root_ + QStringLiteral("/no-such-git");
        QCOMPARE(checkGit(tool), prepare::gitUnsupported);
        QCOMPARE(prepareSnapshot(request(repo_), tool).code, prepare::gitUnsupported);
    }

    void aMalformedInFlightRefFailsTheBuild() {
        PrepareRequest bad = request(repo_);
        bad.inFlightRef = QStringLiteral("../escape");
        QCOMPARE(prepareSnapshot(bad).code, prepare::snapshotBuildFailed);
    }

    void storesAreNamedByRepository() {
        const QString a = storePathFor(stores_, QStringLiteral("/a/.git"));
        QCOMPARE(a, storePathFor(stores_, QStringLiteral("/a/.git")));
        QVERIFY(a != storePathFor(stores_, QStringLiteral("/b/.git")));
        QCOMPARE(QFileInfo(a).fileName().size(), 36);
    }

  private:
    PrepareRequest request(const QString& cwd) const {
        PrepareRequest request;
        request.cwd = cwd;
        request.storesRoot = stores_;
        request.inFlightRef = QStringLiteral("inflight/job-1");
        request.userGitSources =
            snapshot::UserGitSources{home_, home_ + QStringLiteral("/.config"), {}};
        return request;
    }

    std::unique_ptr<QTemporaryDir> dir_;
    QString root_;
    QString home_;
    QString repo_;
    QString stores_;
    QString head_;
};

QTEST_GUILESS_MAIN(SnapshotPreparationTest)
#include "SnapshotPreparationTest.moc"
