#include "CloudPrivacyTestAccess.h"
#include "FakeTransport.h"
#include "MemoryCredentials.h"
#include "cloud/CloudControlSupport.h"
#include "cloud/WorktreeBranches.h"
#include "cloud/WorktreeRepositories.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;

namespace {
using Access = CloudPrivacyTestAccess;
const QString heartbeatRoute = QStringLiteral("/v2/daemon/heartbeat");
const QByteArray pathSalt(32, 's');

struct Fixture {
    QTemporaryDir directory;
    QDir root{QFileInfo(directory.path()).canonicalFilePath()};
    LocalJournal journal{root.filePath("journal.sqlite")};
    CodexAdapter provider{root.filePath("no-provider")};
    SessionHost host{&journal, &provider};
    r03::Transport transport;
    CloudControlClient client{&journal, &host};

    Fixture() {
        Access::configure(client, transport);
        Access::pathSalt(client, pathSalt);
        transport.statusForPath.insert(heartbeatRoute, 202);
    }

    QList<QJsonArray> heartbeatCatalogs() const {
        QList<QJsonArray> catalogs;
        for (qsizetype i = 0; i < transport.requests.size(); ++i) {
            if (transport.requests.at(i).url().path() == heartbeatRoute) {
                const auto body = QJsonDocument::fromJson(transport.bodies.at(i)).object();
                catalogs.append(body.value("worktrees").toArray());
            }
        }
        return catalogs;
    }

    bool rememberConversation(const QString& id, const QString& path) {
        StoredSession session;
        session.id = id;
        session.worktreePath = path;
        session.adapter = "codex";
        session.state = "ready";
        session.generation = 1;
        return journal.upsertSession(session);
    }
};

bool writeMarker(const QString& path, const QByteArray& value) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(value) == value.size();
}

bool linkedCheckout(const QString& repository, const QString& checkout) {
    const QString administration =
        QDir(repository).filePath(".git/worktrees/" + QFileInfo(checkout).fileName());
    return QDir().mkpath(administration) && QDir().mkpath(checkout) &&
           writeMarker(QDir(checkout).filePath(".git"),
                       "gitdir: " + administration.toUtf8() + '\n') &&
           writeMarker(QDir(administration).filePath("gitdir"), checkout.toUtf8() + "/.git\n");
}

QStringList worktreeIds(const QJsonArray& entries) {
    QStringList ids;
    for (const QJsonValue& entry : entries)
        ids.append(entry.toObject().value("worktreeId").toString());
    return ids;
}

QString worktreeId(const QString& path) {
    return cloud_control::opaquePathId("worktree", path, pathSalt);
}

// Membership and order before conversation priority, for fixtures with no live conversation.
QStringList legacyWorktreeIds(const LocalJournal& journal) {
    QStringList paths;
    QStringList repositories;
    const auto add = [&paths](const QString& path) {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (!canonical.isEmpty() && QFileInfo(canonical).isDir() && !paths.contains(canonical) &&
            paths.size() < 100)
            paths.append(canonical);
    };
    for (const QString& enrolled : journal.worktreePaths()) {
        add(enrolled);
        const QString canonical = QFileInfo(enrolled).canonicalFilePath();
        if (!canonical.isEmpty()) {
            const QString repository = worktreeRepository(canonical);
            if (!repositories.contains(repository))
                repositories.append(repository);
        }
    }
    for (const QString& repository : repositories)
        for (const QString& checkout : repositoryCheckouts(repository))
            add(checkout);

    QStringList ids;
    for (const auto& path : paths)
        ids.append(worktreeId(path));
    return ids;
}
} // namespace

class HeartbeatWorktreeCatalogTest final : public QObject {
    Q_OBJECT

  private slots:
    void init() { r03::resetCredentials(); }

    void legacyMembershipAndOrderWithoutConversations_data() {
        QTest::addColumn<int>("existing");
        QTest::addColumn<int>("missing");
        QTest::newRow("small") << 4 << 2;
        QTest::newRow("missing-across-several-pages") << 4 << 259;
        QTest::newRow("full-after-missing-rows") << 125 << 159;
    }

    void legacyMembershipAndOrderWithoutConversations() {
        QFETCH(int, existing);
        QFETCH(int, missing);
        Fixture fixture;
        QVERIFY(fixture.directory.isValid() && fixture.journal.isOpen());
        for (int i = 0; i < missing; ++i) {
            const QString path =
                fixture.root.filePath(QStringLiteral("a-missing-%1").arg(i, 3, 10, u'0'));
            QVERIFY(fixture.journal.rememberWorktree(path));
        }
        for (int i = 0; i < existing; ++i) {
            const QString path =
                fixture.root.filePath(QStringLiteral("b-existing-%1").arg(i, 3, 10, u'0'));
            QVERIFY(QDir().mkpath(path));
            QVERIFY(fixture.journal.rememberWorktree(path));
        }
        const QString gone = fixture.root.filePath("gone");
        const QString orphan = fixture.root.filePath("orphan");
        QVERIFY(linkedCheckout(gone, orphan));
        QVERIFY(QDir(gone).removeRecursively());
        QVERIFY(fixture.journal.rememberWorktree(gone));
        QVERIFY(fixture.journal.rememberWorktree(orphan));
        QVERIFY(fixture.rememberConversation("deleted-worktree", gone));

        const QString primary = fixture.root.filePath("z-primary");
        const QString linked = fixture.root.filePath("z-linked");
        QVERIFY(linkedCheckout(primary, linked));
        QVERIFY(fixture.journal.rememberWorktree(primary));

        const QStringList actual = worktreeIds(Access::worktreeSnapshots(fixture.client));
        QCOMPARE(actual, legacyWorktreeIds(fixture.journal));
        if (existing < 100) {
            QVERIFY(actual.contains(worktreeId(orphan)));
            QVERIFY(actual.contains(worktreeId(linked)));
            QVERIFY(!actual.contains(worktreeId(gone)));
        }
    }

    void conversationsAndTheirCheckoutsLeadAFullCatalog_data() {
        QTest::addColumn<int>("enrolled");
        QTest::newRow("room-for-every-checkout") << 4;
        QTest::newRow("more-enrolled-than-the-limit") << 105;
    }

    void conversationsAndTheirCheckoutsLeadAFullCatalog() {
        QFETCH(int, enrolled);
        Fixture fixture;
        QVERIFY(fixture.directory.isValid() && fixture.journal.isOpen());
        for (int i = 0; i < enrolled; ++i) {
            const QString path =
                fixture.root.filePath(QStringLiteral("a-enrolled-%1").arg(i, 3, 10, u'0'));
            QVERIFY(QDir().mkpath(path));
            QVERIFY(fixture.journal.rememberWorktree(path));
        }
        const QString primary = fixture.root.filePath("z-primary");
        const QString conversation = fixture.root.filePath("z-conversation");
        const QString sibling = fixture.root.filePath("z-sibling");
        QVERIFY(linkedCheckout(primary, conversation));
        QVERIFY(linkedCheckout(primary, sibling));
        QVERIFY(fixture.rememberConversation("session-a", conversation));
        QVERIFY(fixture.rememberConversation("session-b", conversation));

        const auto first = Access::worktreeSnapshots(fixture.client);
        QCOMPARE(first.size(), qMin(100, enrolled + 3));
        const auto ids = worktreeIds(first);
        QCOMPARE(ids.mid(0, 3),
                 (QStringList{worktreeId(conversation), worktreeId(primary), worktreeId(sibling)}));
        QCOMPARE(ids.at(3), worktreeId(fixture.root.filePath("a-enrolled-000")));
        QCOMPARE(Access::worktreeSnapshots(fixture.client), first);
        if (enrolled < 100) {
            auto sorted = ids;
            auto legacy = legacyWorktreeIds(fixture.journal);
            sorted.sort();
            legacy.sort();
            QCOMPARE(sorted, legacy);
        }
        for (const QJsonValue& entry : first) {
            const auto fields = entry.toObject();
            QCOMPARE(fields.keys(),
                     (QStringList{"catalogRevision", "lifecycleRevision", "repositoryId",
                                  "repositoryLabel", "worktreeId", "worktreeLabel"}));
            QCOMPARE(fields.value("catalogRevision").toInt(-1), 0);
            QCOMPARE(fields.value("lifecycleRevision").toInt(-1), 0);
            QCOMPARE(fields.value("worktreeId").toString().size(), 64);
            QCOMPARE(fields.value("repositoryId").toString().size(), 64);
        }
    }

    void heartbeatReusesCatalogUntilItsNextRun() {
        Fixture fixture;
        QVERIFY(fixture.directory.isValid() && fixture.journal.isOpen());
        const QString original = fixture.root.filePath("original");
        const QString replacement = fixture.root.filePath("replacement");
        QVERIFY(QDir().mkpath(original));
        for (int i = 0; i < 101; ++i)
            QVERIFY(fixture.rememberConversation(QStringLiteral("session-%1").arg(i), original));

        fixture.transport.heldPaths.insert(heartbeatRoute);
        Access::publishHeartbeat(fixture.client);
        QTRY_COMPARE(fixture.heartbeatCatalogs().size(), 1);
        const auto first = fixture.heartbeatCatalogs().constFirst();
        QCOMPARE(worktreeIds(first), QStringList{worktreeId(original)});

        QVERIFY(QDir().rmdir(original));
        QVERIFY(QDir().mkpath(replacement));
        QVERIFY(fixture.journal.rememberWorktree(replacement));
        fixture.transport.heldPaths.remove(heartbeatRoute);
        fixture.transport.release(heartbeatRoute);
        QTRY_VERIFY(Access::heartbeatIdle(fixture.client));
        QCOMPARE(fixture.heartbeatCatalogs().size(), 2);
        QCOMPARE(fixture.heartbeatCatalogs().at(1), first);

        Access::publishHeartbeat(fixture.client);
        QTRY_COMPARE(fixture.heartbeatCatalogs().size(), 4);
        QTRY_VERIFY(Access::heartbeatIdle(fixture.client));
        const auto catalogs = fixture.heartbeatCatalogs();
        QCOMPARE(worktreeIds(catalogs.at(2)), QStringList{worktreeId(replacement)});
        QCOMPARE(catalogs.at(3), catalogs.at(2));
    }
};

QTEST_GUILESS_MAIN(HeartbeatWorktreeCatalogTest)
#include "HeartbeatWorktreeCatalogTest.moc"
