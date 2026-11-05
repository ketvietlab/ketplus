#include "app/RepositoryMetadataStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

namespace {
// What `git worktree add` leaves in a linked checkout: a `.git` file naming its Git dir.
bool linkWorktree(const QString& checkout, const QString& identity) {
    const QString name = QFileInfo(checkout).fileName();
    if (!QDir().mkpath(identity + "/worktrees/" + name) || !QDir().mkpath(checkout))
        return false;
    QFile file(checkout + "/.git");
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(("gitdir: " + identity + "/worktrees/" + name + "\n").toUtf8()) > 0;
}
} // namespace

class RepositoryMetadataTest final : public QObject {
    Q_OBJECT
  private slots:
    void persistsAcrossConnectionsAndUpdatesMapping() {
        QTemporaryDir directory;
        const auto database = directory.filePath("cache.sqlite");
        const auto root = directory.filePath("repo");
        const auto identity = root + "/.git";
        const auto linked = directory.filePath("feature");
        QVERIFY(QDir().mkpath(identity));
        QVERIFY(linkWorktree(linked, identity));
        ketplus::RepositoryMetadataStore(database).save({linked, identity, root});
        auto cached = ketplus::RepositoryMetadataStore(database).lookup(linked);
        QVERIFY(cached);
        QCOMPARE(cached->root, root);
        QCOMPARE(cached->identity, identity);
        // Same folder re-associated with a different repository after validation.
        const auto second = directory.filePath("other");
        QVERIFY(QDir().mkpath(second + "/.git"));
        QVERIFY(linkWorktree(linked, second + "/.git"));
        ketplus::RepositoryMetadataStore(database).save({linked, second + "/.git", second});
        cached = ketplus::RepositoryMetadataStore(database).lookup(linked);
        QVERIFY(cached);
        QCOMPARE(cached->root, second);
        ketplus::RepositoryMetadataStore(database).remove(linked);
        QVERIFY(!ketplus::RepositoryMetadataStore(database).lookup(linked));
    }

    void missingAndMovedPathsDoNotReturnStaleMetadata() {
        QTemporaryDir directory;
        const auto root = directory.filePath("repo");
        const auto linked = directory.filePath("feature");
        QVERIFY(QDir().mkpath(root + "/.git"));
        QVERIFY(linkWorktree(linked, root + "/.git"));
        ketplus::RepositoryMetadataStore store(directory.filePath("cache.sqlite"));
        store.save({linked, root + "/.git", root});
        QVERIFY(store.lookup(linked));
        QVERIFY(QDir().rename(linked, linked + "-moved"));
        QVERIFY(!store.lookup(linked));
        QVERIFY(QDir().rename(linked + "-moved", linked));
        QVERIFY(QDir().rename(root + "/.git", root + "/git-backup"));
        QVERIFY(!store.lookup(linked));
    }

    void replacedCheckoutDoesNotReuseItsOldIdentity() {
        QTemporaryDir directory;
        const auto root = directory.filePath("repo");
        const auto other = directory.filePath("other");
        const auto linked = directory.filePath("feature");
        QVERIFY(QDir().mkpath(root + "/.git"));
        QVERIFY(QDir().mkpath(other + "/.git"));
        QVERIFY(linkWorktree(linked, root + "/.git"));
        QVERIFY(QDir().mkpath(linked + "/src"));
        ketplus::RepositoryMetadataStore store(directory.filePath("cache.sqlite"));
        // The row repositoryRoot() writes for a linked worktree: its primary root and identity.
        store.save({linked, root + "/.git", root});
        store.save({linked + "/src", root + "/.git", root});
        QVERIFY(store.lookup(linked));
        QVERIFY(store.lookup(linked + "/src"));

        // The same path recreated as a worktree of another repository.
        QVERIFY(linkWorktree(linked, other + "/.git"));
        QVERIFY(!store.lookup(linked));
        QVERIFY(!store.lookup(linked + "/src"));

        // A main checkout replaced by an unrelated clone at the same path.
        store.save({root, root + "/.git", root});
        QVERIFY(store.lookup(root));
        QVERIFY(QDir(root + "/.git").removeRecursively());
        QVERIFY(linkWorktree(root, other + "/.git"));
        QVERIFY(!store.lookup(root));
        QCOMPARE(ketplus::RepositoryMetadataStore::checkoutRoot(linked + "/src"),
                 QFileInfo(linked).canonicalFilePath());
    }

    void batchLookupReturnsOnlyValidEntries() {
        QTemporaryDir directory;
        const auto root = directory.filePath("repo");
        const auto linked = directory.filePath("feature");
        const auto moved = directory.filePath("moved");
        const auto unknown = directory.filePath("unknown");
        QVERIFY(QDir().mkpath(root + "/.git"));
        QVERIFY(linkWorktree(linked, root + "/.git"));
        QVERIFY(linkWorktree(moved, root + "/.git"));
        QVERIFY(QDir().mkpath(unknown));
        ketplus::RepositoryMetadataStore store(directory.filePath("cache.sqlite"));
        store.save({root, root + "/.git", root});
        store.save({linked, root + "/.git", root});
        store.save({moved, root + "/.git", root});
        QVERIFY(QDir().rename(moved, moved + "-elsewhere"));
        const auto found = store.lookupAll({root, linked, moved, unknown, root, QString{}});
        QCOMPARE(found.size(), 2);
        QCOMPARE(found.value(root).root, root);
        QCOMPARE(found.value(linked).root, root);
        QCOMPARE(found.value(linked).identity, root + "/.git");
        QVERIFY(!found.contains(moved));
        QVERIFY(!found.contains(unknown));
        QVERIFY(ketplus::RepositoryMetadataStore(directory.filePath("absent/cache.sqlite"))
                    .lookupAll({root})
                    .isEmpty());
    }

    void unavailableDatabaseIsACacheMiss() {
        QTemporaryDir directory;
        ketplus::RepositoryMetadataStore missing(directory.filePath("absent/cache.sqlite"));
        QVERIFY(!missing.lookup(directory.path()));
        QVERIFY(!QFileInfo::exists(directory.filePath("absent")));
        QFile corrupt(directory.filePath("corrupt.sqlite"));
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        corrupt.write("not a SQLite database");
        corrupt.close();
        ketplus::RepositoryMetadataStore store(corrupt.fileName());
        store.save({directory.path(), directory.path(), directory.path()});
        QVERIFY(!store.lookup(directory.path()));
    }
};

QTEST_GUILESS_MAIN(RepositoryMetadataTest)
#include "RepositoryMetadataTest.moc"
