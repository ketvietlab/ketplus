#include "snapshot/SnapshotStore.h"
#include "snapshot/WorktreeSnapshot.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QMap>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <sys/time.h>

using namespace ketplus::runner::snapshot;

namespace {

// Fixture git with a private HOME, so no developer configuration shapes the repositories.
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

void setMtime(const QString& path, const QDateTime& when) {
    const qint64 seconds = when.toSecsSinceEpoch();
    const timeval times[2] = {{static_cast<time_t>(seconds), 0}, {static_cast<time_t>(seconds), 0}};
    QCOMPARE(utimes(QFile::encodeName(path).constData(), times), 0);
}

// Every file under `root` with its size, mtime and content digest.
QMap<QString, QString> fingerprint(const QString& root) {
    QMap<QString, QString> files;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        const bool opened = file.open(QIODevice::ReadOnly);
        const QFileInfo info(path);
        files.insert(
            path,
            QString::number(opened) + ' ' + QString::number(info.size()) + ' ' +
                QString::number(info.lastModified().toMSecsSinceEpoch()) + ' ' +
                QString::fromLatin1(
                    QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex()));
    }
    return files;
}

// The store's loose object file of `id`.
QString looseObject(const QString& store, const QString& id) {
    return QDir(store).filePath(QStringLiteral("objects/%1/%2").arg(id.left(2), id.mid(2)));
}

bool storeHas(const QString& home, const QString& store, const QString& id) {
    bool ok = false;
    git(home, store,
        {QStringLiteral("--git-dir=") + store, QStringLiteral("cat-file"), QStringLiteral("-e"),
         id},
        &ok);
    return ok;
}

} // namespace

class SnapshotStoreTest final : public QObject {
    Q_OBJECT

    QTemporaryDir temp_;
    QString home_;
    QString repo_;
    QString store_;

    Snapshot snapshotOf(const QByteArray& untracked) {
        write(repo_ + QStringLiteral("/scratch.txt"), untracked);
        auto made = createSnapshot({repo_, store_, UserGitSources{home_, home_ + "/.config", {}}});
        if (!made)
            qWarning() << made.error().message;
        return made ? made.value() : Snapshot{};
    }

  private slots:
    void init() {
        QVERIFY(temp_.isValid());
        const QString root = temp_.path() + QStringLiteral("/") + QTest::currentTestFunction();
        home_ = root + QStringLiteral("/home");
        repo_ = root + QStringLiteral("/repo");
        store_ = root + QStringLiteral("/store.git");
        QDir().mkpath(home_);
        QDir().mkpath(repo_);
        git(home_, repo_, {QStringLiteral("init"), QStringLiteral("-q")});
        write(repo_ + QStringLiteral("/a.txt"), "one\n");
        git(home_, repo_, {QStringLiteral("add"), QStringLiteral("a.txt")});
        git(home_, repo_, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("one")});
    }

    void refNamesAreChecked_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("valid");
        QTest::newRow("confirmed") << "confirmed/runner-a/key_1" << true;
        QTest::newRow("in flight") << "sending/job-1" << true;
        QTest::newRow("inner dot") << "a/b.c" << true;
        QTest::newRow("empty") << "" << false;
        QTest::newRow("parent") << "a/../b" << false;
        QTest::newRow("leading slash") << "/a" << false;
        QTest::newRow("empty segment") << "a//b" << false;
        QTest::newRow("leading dot") << ".a" << false;
        QTest::newRow("lock") << "a.lock" << false;
        QTest::newRow("space") << "a b" << false;
        QTest::newRow("too long") << QString(201, QLatin1Char('a')) << false;
        QTest::newRow("longest") << QString(200, QLatin1Char('a')) << true;
    }
    void refNamesAreChecked() {
        QFETCH(QString, name);
        QFETCH(bool, valid);
        QCOMPARE(isStoreRefName(name), valid);
    }

    void refsAreKeptListedAndDroppedInTheStoreOnly() {
        const Snapshot snapshot = snapshotOf("x\n");
        QVERIFY(!snapshot.commit.isEmpty());
        const auto userRefs = fingerprint(repo_ + QStringLiteral("/.git/refs"));
        QVERIFY(keepRef(store_, QStringLiteral("sending/job-1"), snapshot.commit).ok());
        QVERIFY(keepRef(store_, QStringLiteral("confirmed/runner-a/key"), snapshot.commit).ok());
        auto refs = listRefs(store_);
        QVERIFY(refs.ok());
        QCOMPARE(refs.value(), (QMap<QString, QString>{{"confirmed/runner-a/key", snapshot.commit},
                                                       {"sending/job-1", snapshot.commit}}));
        QVERIFY(dropRef(store_, QStringLiteral("sending/job-1")).ok());
        QVERIFY(dropRef(store_, QStringLiteral("sending/job-1")).ok()); // already gone
        QCOMPARE(listRefs(store_).value().keys(), QStringList{"confirmed/runner-a/key"});
        QCOMPARE(fingerprint(repo_ + QStringLiteral("/.git/refs")), userRefs);

        QCOMPARE(keepRef(store_, QStringLiteral("a/../b"), snapshot.commit).error().code,
                 ErrorCode::InvalidArgument);
        QCOMPARE(keepRef(store_, QStringLiteral("x"), QString(40, QLatin1Char('f'))).error().code,
                 ErrorCode::InvalidArgument);
        QCOMPARE(keepRef(store_, QStringLiteral("x"), QStringLiteral("HEAD")).error().code,
                 ErrorCode::InvalidArgument);
        QCOMPARE(listRefs(temp_.path() + QStringLiteral("/none")).error().code,
                 ErrorCode::InvalidArgument);
    }

    void pruneRemovesOnlyOldUnreferencedStoreObjects() {
        const Snapshot kept = snapshotOf("kept\n");
        const Snapshot dropped = snapshotOf("dropped\n");
        QVERIFY(!kept.commit.isEmpty() && !dropped.commit.isEmpty() &&
                kept.commit != dropped.commit);
        QVERIFY(keepRef(store_, QStringLiteral("confirmed/r/k"), kept.commit).ok());
        const QDateTime expire = QDateTime::currentDateTimeUtc().addDays(-1);

        // Just after the cut-off: kept.
        QDirIterator objects(store_ + QStringLiteral("/objects"), QDir::Files,
                             QDirIterator::Subdirectories);
        while (objects.hasNext())
            setMtime(objects.next(), expire.addSecs(10));
        const auto userObjects = fingerprint(repo_ + QStringLiteral("/.git"));
        QVERIFY(pruneStore(store_, expire).ok());
        QVERIFY(storeHas(home_, store_, dropped.commit));

        // Just before the cut-off: the unreferenced snapshot goes, the kept one stays.
        QDirIterator again(store_ + QStringLiteral("/objects"), QDir::Files,
                           QDirIterator::Subdirectories);
        while (again.hasNext())
            setMtime(again.next(), expire.addSecs(-10));
        QVERIFY(pruneStore(store_, expire).ok());
        QVERIFY(!QFileInfo::exists(looseObject(store_, dropped.commit)));
        QVERIFY(QFileInfo::exists(looseObject(store_, kept.commit)));
        QVERIFY(writePack({store_, kept.commit, {}, temp_.path() + "/kept.pack"}).ok());
        QCOMPARE(fingerprint(repo_ + QStringLiteral("/.git")), userObjects);
        QCOMPARE(pruneStore(store_, QDateTime()).error().code, ErrorCode::InvalidArgument);
    }

    void aSnapshotMissingAnObjectIsIncomplete() {
        const Snapshot snapshot = snapshotOf("whole\n");
        QVERIFY(!snapshot.commit.isEmpty());
        QCOMPARE(snapshotComplete(store_, snapshot.commit).value(), true);
        QVERIFY(QFile::remove(looseObject(store_, snapshot.tree)));
        QCOMPARE(snapshotComplete(store_, snapshot.commit).value(), false);
        QCOMPARE(snapshotComplete(store_, QString(40, QLatin1Char('f'))).value(), false);
        QCOMPARE(snapshotComplete(store_, QStringLiteral("HEAD")).error().code,
                 ErrorCode::InvalidArgument);
    }

    void sourceHeadFollowsTheBranch() {
        auto layout = readRepositoryLayout(repo_);
        QVERIFY(layout.ok());
        const QString head = QString::fromLatin1(git(home_, repo_, {"rev-parse", "HEAD"}));
        SourceHead read = readSourceHead(layout.value());
        QCOMPARE(read.commit, head);
        QVERIFY(!read.unborn && !read.unreadable);

        git(home_, repo_, {"pack-refs", "--all", "--prune"});
        QVERIFY(!QFileInfo::exists(repo_ + "/.git/refs/heads/main"));
        QCOMPARE(readSourceHead(layout.value()).commit, head);

        git(home_, repo_, {"checkout", "-q", "--detach"});
        QCOMPARE(readSourceHead(layout.value()).commit, head);

        git(home_, repo_, {"checkout", "-q", "--orphan", "fresh"});
        read = readSourceHead(layout.value());
        QVERIFY(read.commit.isEmpty() && read.unborn && !read.unreadable);
    }

    void sourceHeadOfALinkedWorktree() {
        const QString linked = QFileInfo(repo_).path() + QStringLiteral("/linked");
        git(home_, repo_, {"branch", "side"});
        write(repo_ + "/b.txt", "two\n");
        git(home_, repo_, {"add", "b.txt"});
        git(home_, repo_, {"commit", "-qm", "two"});
        git(home_, repo_, {"worktree", "add", "-q", linked, "side"});
        auto layout = readRepositoryLayout(linked);
        QVERIFY(layout.ok());
        QCOMPARE(readSourceHead(layout.value()).commit,
                 QString::fromLatin1(git(home_, linked, {"rev-parse", "HEAD"})));
        QVERIFY(readSourceHead(layout.value()).commit !=
                QString::fromLatin1(git(home_, repo_, {"rev-parse", "HEAD"})));
    }

    void anUnreadableHeadIsEmpty_data() {
        QTest::addColumn<QByteArray>("head");
        QTest::newRow("outside refs") << QByteArray("ref: ../../outside\n");
        QTest::newRow("absolute") << QByteArray("ref: /etc/passwd\n");
        QTest::newRow("parent inside refs") << QByteArray("ref: refs/../../config\n");
        QTest::newRow("short id") << QByteArray("abc123\n");
        QTest::newRow("upper case id") << QByteArray(QByteArray(40, 'A') + '\n');
        QTest::newRow("garbage") << QByteArray("hello\n");
    }
    void anUnreadableHeadIsEmpty() {
        QFETCH(QByteArray, head);
        write(repo_ + "/.git/HEAD", head);
        auto layout = readRepositoryLayout(repo_);
        if (!layout.ok())
            return; // the layout reader refuses it first
        const SourceHead read = readSourceHead(layout.value());
        QVERIFY(read.commit.isEmpty());
        QVERIFY(read.unreadable);
    }

    void aReftableRepositoryIsUnreadable() {
        QDir().mkpath(repo_ + "/.git/reftable");
        auto layout = readRepositoryLayout(repo_);
        QVERIFY(layout.ok());
        const SourceHead read = readSourceHead(layout.value());
        QVERIFY(read.commit.isEmpty() && read.unreadable);
    }

    void aSymlinkedRefIsUnreadable() {
        const QString target = repo_ + "/.git/refs/heads/main";
        const QString moved = temp_.path() + "/moved-ref";
        QVERIFY(QFile::rename(target, moved));
        QVERIFY(QFile::link(moved, target));
        auto layout = readRepositoryLayout(repo_);
        QVERIFY(layout.ok());
        QVERIFY(readSourceHead(layout.value()).unreadable);
    }
};

QTEST_GUILESS_MAIN(SnapshotStoreTest)
#include "SnapshotStoreTest.moc"
