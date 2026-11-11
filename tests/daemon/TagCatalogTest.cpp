#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include "AgentTestFixture.h"

#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;
using agent_test::request;

namespace {

QJsonObject tag(const QString& label, const QString& color) {
    return {{"label", label}, {"color", color}};
}

QStringList labels(const QJsonArray& catalog) {
    QStringList result;
    for (const auto& value : catalog)
        result.append(value.toObject().value("label").toString());
    return result;
}

StoredSession storedSession(const QString& id, const QString& path, const QStringList& tags) {
    StoredSession session;
    session.id = id;
    session.worktreePath = path;
    session.adapter = "codex";
    session.state = "ready";
    session.tags = tags;
    return session;
}

} // namespace

// #429: the tag catalog the daemon keeps for every desktop window and the phone.
class TagCatalogTest final : public QObject {
    Q_OBJECT

  private slots:
    void catalogStartsWithTheDefaultsAndSurvivesRestart() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("journal.sqlite3");
        QVERIFY(protocol::features(1).contains("tags.catalog/1"));
        {
            LocalJournal journal(path);
            CodexAdapter adapter("/missing-codex-for-tags");
            SessionHost host(&journal, &adapter);
            const QJsonObject listed = host.handleCommand(request("tags.catalog"), "desktop-test");
            QVERIFY(listed.value("ok").toBool());
            QCOMPARE(
                labels(listed.value("tags").toArray()),
                QStringList({"UI", "Performance", "Review", "Atlas", "Account", "Bug", "Docs"}));
            const QJsonObject saved = host.handleCommand(
                request("tags.catalog.set",
                        {{"tags", QJsonArray{tag("Release", "#D1453B"), tag("UI", "#4f6bed")}}}),
                "desktop-test");
            QVERIFY2(saved.value("ok").toBool(),
                     QJsonDocument(saved).toJson(QJsonDocument::Compact).constData());
        }
        LocalJournal journal(path);
        CodexAdapter adapter("/missing-codex-for-tags");
        SessionHost host(&journal, &adapter);
        const QJsonArray catalog =
            host.handleCommand(request("tags.catalog"), "desktop-test").value("tags").toArray();
        QCOMPARE(labels(catalog), QStringList({"Release", "UI"}));
        QCOMPARE(catalog.first().toObject().value("color").toString(), "#d1453b");
        // An empty catalog is kept as empty, not replaced by the defaults.
        QVERIFY(host.handleCommand(request("tags.catalog.set", {{"tags", QJsonArray{}}}),
                                   "desktop-test")
                    .value("ok")
                    .toBool());
        QVERIFY(journal.tagCatalog().isEmpty());
    }

    void invalidCatalogsAreRefused_data() {
        QTest::addColumn<QJsonObject>("payload");
        QTest::newRow("not-array") << QJsonObject{{"tags", "UI"}};
        QTest::newRow("empty-label") << QJsonObject{{"tags", QJsonArray{tag(" ", "#4f6bed")}}};
        QTest::newRow("bad-colour") << QJsonObject{{"tags", QJsonArray{tag("UI", "blue")}}};
        QTest::newRow("duplicate")
            << QJsonObject{{"tags", QJsonArray{tag("UI", "#4f6bed"), tag("ui", "#c2780a")}}};
        QTest::newRow("control") << QJsonObject{{"tags", QJsonArray{tag("a\nb", "#4f6bed")}}};
        QTest::newRow("rename-to-missing")
            << QJsonObject{{"tags", QJsonArray{tag("UI", "#4f6bed")}},
                           {"renames", QJsonObject{{"Bug", "Defect"}}}};
        QJsonArray tooMany;
        for (int index = 0; index < 33; ++index)
            tooMany.append(tag(QString("tag-%1").arg(index), "#4f6bed"));
        QTest::newRow("too-many") << QJsonObject{{"tags", tooMany}};
    }

    void invalidCatalogsAreRefused() {
        QFETCH(QJsonObject, payload);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        CodexAdapter adapter("/missing-codex-for-tags");
        SessionHost host(&journal, &adapter);
        const QJsonObject result =
            host.handleCommand(request("tags.catalog.set", payload), "desktop-test");
        QCOMPARE(result.value("error_code").toString(), "invalid_payload");
        QCOMPARE(labels(journal.tagCatalog()).size(), 7);
    }

    // Renaming keeps the label on its conversations; removing takes it off; every window
    // hears both at once through `tags.changed`.
    void renamesAndRemovalsReachEveryConversation() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(journal.upsertSession(storedSession("first", directory.path(), {})));
        QVERIFY(journal.upsertSession(storedSession("second", directory.path(), {})));
        QVERIFY(journal.upsertSession(storedSession("untouched", directory.path(), {})));
        QVERIFY(journal.setSessionTags("first", 1, {"Bug", "UI"}));
        QVERIFY(journal.setSessionTags("second", 1, {"Docs"}));
        QVERIFY(journal.setSessionTags("untouched", 1, {"Review"}));
        CodexAdapter adapter("/missing-codex-for-tags");
        SessionHost host(&journal, &adapter);
        QSignalSpy notices(&host, &SessionHost::noticeReady);

        const QJsonObject saved = host.handleCommand(
            request("tags.catalog.set",
                    {{"tags", QJsonArray{tag("UI", "#4f6bed"), tag("Defect", "#d1453b"),
                                         tag("Review", "#7c5cd6")}},
                     {"renames", QJsonObject{{"bug", "Defect"}}}}),
            "desktop-test");
        QVERIFY2(saved.value("ok").toBool(),
                 QJsonDocument(saved).toJson(QJsonDocument::Compact).constData());
        QHash<QString, QStringList> tags;
        for (const StoredSession& session : journal.sessions())
            tags.insert(session.id, session.tags);
        QCOMPARE(tags.value("first"), QStringList({"Defect", "UI"}));
        QCOMPARE(tags.value("second"), QStringList());
        QCOMPARE(tags.value("untouched"), QStringList({"Review"}));

        QCOMPARE(notices.size(), 1);
        QCOMPARE(notices.first().at(0).toString(), "tags.changed");
        const QJsonObject notice = notices.first().at(1).toJsonObject();
        QCOMPARE(labels(notice.value("catalog").toArray()),
                 QStringList({"UI", "Defect", "Review"}));
        QCOMPARE(notice.value("sessions").toArray().size(), 2);

        // Tagging one conversation is heard by the other windows too.
        notices.clear();
        QVERIFY(host.handleCommand(
                        request("session.tags.set", {{"tags", QJsonArray{"Review"}}}, "second", 1),
                        "desktop-test")
                    .value("ok")
                    .toBool());
        QCOMPARE(notices.size(), 1);
        const QJsonArray sessions =
            notices.first().at(1).toJsonObject().value("sessions").toArray();
        QCOMPARE(sessions.first().toObject().value("session_id").toString(), "second");
        QCOMPARE(sessions.first().toObject().value("tags").toArray(), QJsonArray{"Review"});
        QVERIFY(!notices.first().at(1).toJsonObject().contains("catalog"));
    }
};

QTEST_GUILESS_MAIN(TagCatalogTest)
#include "TagCatalogTest.moc"
