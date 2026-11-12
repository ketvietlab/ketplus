#include "cloud/WorktreeRevisionCache.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;

namespace {
StoredSession session(const QString& id, const QString& worktree, const qint64 cursor) {
    StoredSession stored;
    stored.id = id;
    stored.worktreePath = worktree;
    stored.state = QStringLiteral("ready");
    stored.lastCursor = cursor;
    return stored;
}

void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
}
} // namespace

class WorktreeRevisionCacheTest final : public QObject {
    Q_OBJECT

  private slots:
    // The heartbeat runs git for a worktree again only when a conversation on it, git's HEAD or
    // index moved, or the cached revision has aged out.
    void revisionIsComputedOnlyWhenItMayHaveChanged() {
        QTemporaryDir dir;
        const QString worktree = dir.path();
        QVERIFY(QDir(worktree).mkpath(QStringLiteral(".git")));
        write(QDir(worktree).filePath(QStringLiteral(".git/HEAD")), "ref: refs/heads/main\n");
        write(QDir(worktree).filePath(QStringLiteral(".git/index")), "index-one");

        WorktreeRevisionCache cache;
        int computed = 0;
        const auto compute = [&computed](const QString&) {
            ++computed;
            return QStringLiteral("revision-%1").arg(computed);
        };
        QList<StoredSession> sessions{session(QStringLiteral("a"), worktree, 1)};
        qint64 now = 1'000'000;
        QCOMPARE(cache.revision(worktree, sessions, now, compute), QString("revision-1"));
        QCOMPARE(cache.revision(worktree, sessions, now + 15'000, compute), QString("revision-1"));
        QCOMPARE(computed, 1);

        // A conversation recorded an event.
        sessions[0].lastCursor = 2;
        QCOMPARE(cache.revision(worktree, sessions, now + 30'000, compute), QString("revision-2"));

        // git wrote its index (a commit, a checkout or `git add`).
        write(QDir(worktree).filePath(QStringLiteral(".git/index")), "index-two-longer");
        QCOMPARE(cache.revision(worktree, sessions, now + 45'000, compute), QString("revision-3"));
        QCOMPARE(cache.revision(worktree, sessions, now + 60'000, compute), QString("revision-3"));

        // Edits made outside any conversation are picked up once the revision ages out.
        now += 45'000 + WorktreeRevisionCache::maximumAgeMs;
        QCOMPARE(cache.revision(worktree, sessions, now, compute), QString("revision-4"));
        // A clock that went back never serves a revision from the future.
        QCOMPARE(cache.revision(worktree, sessions, now - 1, compute), QString("revision-5"));
        QCOMPARE(computed, 5);
    }

    // A commit with nothing staged moves HEAD through the branch ref, which only the reflog shows.
    void headMoveWithoutIndexChangeIsNoticed() {
        QTemporaryDir dir;
        const QString worktree = dir.path();
        QVERIFY(QDir(worktree).mkpath(QStringLiteral(".git/logs")));
        write(QDir(worktree).filePath(QStringLiteral(".git/HEAD")), "ref: refs/heads/main\n");
        write(QDir(worktree).filePath(QStringLiteral(".git/logs/HEAD")), "one\n");
        const QList<StoredSession> sessions{session(QStringLiteral("a"), worktree, 1)};
        const QByteArray before = WorktreeRevisionCache::fingerprint(worktree, sessions);
        write(QDir(worktree).filePath(QStringLiteral(".git/logs/HEAD")), "one\ntwo\n");
        QVERIFY(WorktreeRevisionCache::fingerprint(worktree, sessions) != before);
    }

    // A workspace rooted in a folder of a repository uses that repository's git directory.
    void subfolderWorkspaceUsesTheEnclosingRepository() {
        QTemporaryDir dir;
        const QString folder = QDir(dir.path()).filePath(QStringLiteral("packages/app"));
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral(".git")));
        write(QDir(dir.path()).filePath(QStringLiteral(".git/index")), "one");
        const QList<StoredSession> sessions{session(QStringLiteral("a"), folder, 1)};
        const QByteArray before = WorktreeRevisionCache::fingerprint(folder, sessions);
        QVERIFY(!before.endsWith("no-git"));
        write(QDir(dir.path()).filePath(QStringLiteral(".git/index")), "two-longer");
        QVERIFY(WorktreeRevisionCache::fingerprint(folder, sessions) != before);
    }

    // A failed git run (an error or a timeout) is tried again on the next heartbeat; outside a
    // repository an empty revision is the answer and is kept.
    void failedComputationIsNotKept() {
        QTemporaryDir dir;
        const QString repository = QDir(dir.path()).filePath(QStringLiteral("repo"));
        const QString plain = QDir(dir.path()).filePath(QStringLiteral("plain"));
        QVERIFY(QDir().mkpath(repository + QStringLiteral("/.git")));
        QVERIFY(QDir().mkpath(plain));
        // QTemporaryDir may itself sit inside a repository; skip the plain half then.
        int computed = 0;
        const auto fail = [&computed](const QString&) {
            ++computed;
            return QString();
        };
        WorktreeRevisionCache cache;
        const QList<StoredSession> inRepository{session(QStringLiteral("a"), repository, 1)};
        QVERIFY(cache.revision(repository, inRepository, 0, fail).isEmpty());
        QVERIFY(cache.revision(repository, inRepository, 15'000, fail).isEmpty());
        QCOMPARE(computed, 2);
        if (WorktreeRevisionCache::fingerprint(plain, {}).endsWith("no-git")) {
            computed = 0;
            cache.revision(plain, {}, 0, fail);
            cache.revision(plain, {}, 15'000, fail);
            QCOMPARE(computed, 1);
        }
    }

    void linkedWorktreeFollowsItsGitFile() {
        QTemporaryDir dir;
        const QString worktree = QDir(dir.path()).filePath(QStringLiteral("wt"));
        const QString gitDir = QDir(dir.path()).filePath(QStringLiteral("repo/.git/worktrees/wt"));
        QVERIFY(QDir().mkpath(worktree));
        QVERIFY(QDir().mkpath(gitDir));
        write(QDir(worktree).filePath(QStringLiteral(".git")), "gitdir: " + gitDir.toUtf8() + "\n");
        write(QDir(gitDir).filePath(QStringLiteral("HEAD")), "0123456789\n");
        const QList<StoredSession> sessions{session(QStringLiteral("a"), worktree, 1)};
        const QByteArray before = WorktreeRevisionCache::fingerprint(worktree, sessions);
        write(QDir(gitDir).filePath(QStringLiteral("HEAD")), "0123456789abcdef\n");
        QVERIFY(WorktreeRevisionCache::fingerprint(worktree, sessions) != before);
    }

    void sessionOrderDoesNotMatterAndUnusedWorktreesAreForgotten() {
        QTemporaryDir dir;
        const QString worktree = dir.path();
        const StoredSession a = session(QStringLiteral("a"), worktree, 3);
        const StoredSession b = session(QStringLiteral("b"), worktree, 7);
        QCOMPARE(WorktreeRevisionCache::fingerprint(worktree, {a, b}),
                 WorktreeRevisionCache::fingerprint(worktree, {b, a}));

        WorktreeRevisionCache cache;
        const auto compute = [](const QString&) { return QStringLiteral("r"); };
        cache.revision(worktree, {a}, 0, compute);
        cache.revision(QStringLiteral("/missing/worktree"), {}, 0, compute);
        QCOMPARE(cache.size(), 2);
        cache.retain({a});
        QCOMPARE(cache.size(), 1);
        cache.retain({});
        QCOMPARE(cache.size(), 0);
    }
};

QTEST_GUILESS_MAIN(WorktreeRevisionCacheTest)
#include "WorktreeRevisionCacheTest.moc"
