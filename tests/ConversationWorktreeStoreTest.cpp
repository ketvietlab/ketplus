#include "agent/ConversationWorktreeStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

class ConversationWorktreeStoreTest final : public QObject {
    Q_OBJECT

  private slots:
    void persistsVisibleOwnedPathsUntilHiddenOrRemoved() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString database =
            directory.filePath(QStringLiteral("conversation-worktrees.sqlite"));
        const QString primary = directory.filePath(QStringLiteral("repo"));
        const QString extra = directory.filePath(QStringLiteral("repo-agent"));
        QVERIFY(QDir().mkpath(primary));
        QVERIFY(QDir().mkpath(extra));

        ketplus::ConversationWorktreeStore store(database);
        store.save(QStringLiteral("conversation-a"), primary,
                   QString::fromLatin1(ketplus::ConversationWorktreeStore::kRolePrimary));
        store.save(QStringLiteral("conversation-a"), extra,
                   QString::fromLatin1(ketplus::ConversationWorktreeStore::kRoleAgent));
        store.save(QStringLiteral("conversation-b"), directory.filePath(QStringLiteral("other")),
                   QString::fromLatin1(ketplus::ConversationWorktreeStore::kRoleAgent));

        QCOMPARE(store.visiblePaths(QStringLiteral("conversation-a")).size(), 2);
        QVERIFY(!store.containsPath(extra, QStringLiteral("conversation-a")));
        QVERIFY(store.containsPath(extra, QStringLiteral("conversation-b")));

        store.hide(QStringLiteral("conversation-a"), extra);
        QCOMPARE(store.visiblePaths(QStringLiteral("conversation-a")).size(), 1);
        QCOMPARE(QFileInfo(store.visiblePaths(QStringLiteral("conversation-a")).constFirst())
                     .canonicalFilePath(),
                 QFileInfo(primary).canonicalFilePath());
        QCOMPARE(store.allPaths(QStringLiteral("conversation-a")).size(), 2);
        QVERIFY(store.containsPath(extra, QStringLiteral("conversation-b")));

        store.removeConversation(QStringLiteral("conversation-a"));
        QVERIFY(store.allPaths(QStringLiteral("conversation-a")).isEmpty());
        QVERIFY(!store.containsPath(primary));
    }

    void matchesDeletedWorktreesUnderSymlinkedParents() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString real = directory.filePath(QStringLiteral("real"));
        const QString alias = directory.filePath(QStringLiteral("alias"));
        QVERIFY(QDir().mkpath(QDir(real).filePath(QStringLiteral("repo-agent"))));
        QVERIFY(QFile::link(real, alias));
        const QString worktree = QDir(alias).filePath(QStringLiteral("repo-agent"));

        ketplus::ConversationWorktreeStore store(
            directory.filePath(QStringLiteral("conversation-worktrees.sqlite")));
        store.save(QStringLiteral("conversation-a"), worktree,
                   QString::fromLatin1(ketplus::ConversationWorktreeStore::kRoleAgent));
        QCOMPARE(store.visiblePaths(QStringLiteral("conversation-a")).size(), 1);

        // Once the worktree is gone it has no canonical path, but its parent still resolves.
        QVERIFY(QDir(QDir(real).filePath(QStringLiteral("repo-agent"))).removeRecursively());
        QVERIFY(store.containsPath(worktree, QStringLiteral("other-conversation")));
        store.hide(QStringLiteral("conversation-a"), worktree);
        QVERIFY(store.visiblePaths(QStringLiteral("conversation-a")).isEmpty());
    }

    void movedCheckoutCannotTransferOwnershipToItsReplacement() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto git = [&](const QStringList& arguments) {
            QProcess process;
            process.setWorkingDirectory(directory.path());
            process.start("git", arguments);
            return process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit &&
                   process.exitCode() == 0;
        };
        QVERIFY(git({"init", "-b", "main", "repo"}));
        QVERIFY(
            git({"-C", "repo", "-c", "user.name=Fixture", "-c", "user.email=test@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "--allow-empty", "-m", "initial"}));
        QVERIFY(git({"-C", "repo", "worktree", "add", "-b", "original", "../linked"}));
        const QString original = QFileInfo(directory.filePath("linked")).canonicalFilePath();
        const QString moved = directory.filePath("moved");
        const QString database = directory.filePath("conversation-worktrees.sqlite");
        ketplus::ConversationWorktreeStore store(database);
        store.save("conversation-a", original, QString::fromLatin1(store.kRoleAgent));
        QCOMPARE(store.removablePaths("conversation-a"), QStringList{original});
        QCOMPARE(store.visiblePaths("conversation-a"), QStringList{original});

        QVERIFY(git({"-C", "repo", "worktree", "move", original, moved}));
        QCOMPARE(store.visiblePaths("conversation-a"), QStringList{original});
        QVERIFY(store.removablePaths("conversation-a").isEmpty());

        QVERIFY(git({"-C", "repo", "worktree", "add", "-b", "replacement", original}));
        QVERIFY(store.visiblePaths("conversation-a").isEmpty());
        QVERIFY(!store.containsPath(original, "conversation-b"));
        ketplus::ConversationWorktreeStore reopened(database);
        QVERIFY(reopened.removablePaths("conversation-a").isEmpty());
        reopened.save("conversation-b", original, QString::fromLatin1(store.kRoleAgent));
        QCOMPARE(reopened.removablePaths("conversation-b"), QStringList{original});

        QVERIFY(git({"-C", "repo", "worktree", "remove", original}));
        QVERIFY(reopened.removablePaths("conversation-b").isEmpty());
    }

    void createdDuringTurnIgnoresMainAndExistingCheckouts() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("repo"))));
        QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("repo-existing"))));
        QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("repo-created"))));
        const QString main =
            QFileInfo(directory.filePath(QStringLiteral("repo"))).canonicalFilePath();
        const QString existing =
            QFileInfo(directory.filePath(QStringLiteral("repo-existing"))).canonicalFilePath();
        const QString created =
            QFileInfo(directory.filePath(QStringLiteral("repo-created"))).canonicalFilePath();
        QVERIFY(!main.isEmpty() && !existing.isEmpty() && !created.isEmpty());

        QCOMPARE(ketplus::ConversationWorktreeStore::createdDuringTurn(
                     {main, existing}, {main, existing, created, created}, main),
                 QStringList{created});
        QVERIFY(
            ketplus::ConversationWorktreeStore::createdDuringTurn({main}, {main}, main).isEmpty());
    }
};

QTEST_GUILESS_MAIN(ConversationWorktreeStoreTest)
#include "ConversationWorktreeStoreTest.moc"
