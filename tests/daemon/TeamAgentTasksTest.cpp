// T-36: team_tasks for agents: compact rows, a bounded page of one task's history, and one
// entry read whole. With the scripted fake providers; no real provider is started.
#include "AgentTestFixture.h"
#include "teams/Team.h"

#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>

using namespace agent_test;

namespace {

void git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", arguments);
    QVERIFY(process.waitForFinished(15000));
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
}

struct Members final {
    QString leader;
    QString developer;
    QString second;
};

} // namespace

class TeamAgentTasksTest final : public QObject {
    Q_OBJECT

  private:
    Members create(Fixture& fixture) {
        const QString repository = fixture.directory.filePath("shop");
        QDir().mkpath(repository);
        git(repository, {"init", "-q", "-b", "integration"});
        git(repository, {"-c", "user.email=t@example.com", "-c", "user.name=T", "commit", "-q",
                         "--allow-empty", "-m", "start"});
        const QJsonObject created = fixture.host->handleCommand(
            request("team.create",
                    {{"repository", repository},
                     {"name", "Shop"},
                     {"integration_branch", "integration"},
                     {"plan",
                      QJsonObject{
                          {"leader", QJsonObject{{"provider", "claude"}}},
                          {"developers",
                           QJsonObject{
                               {"count", 2},
                               {"provider", "codex"},
                               {"members", QJsonArray{QJsonObject{{"paths", QJsonArray{"web"}}},
                                                      QJsonObject{{"paths", QJsonArray{"api"}}}}}}},
                          {"reviewer", QJsonObject{{"provider", "claude"}}}}}}),
            "desktop-test");
        const QJsonArray members = created.value("team").toObject().value("members").toArray();
        if (members.size() != 4)
            return {};
        return {.leader = members.at(0).toObject().value("session_id").toString(),
                .developer = members.at(1).toObject().value("session_id").toString(),
                .second = members.at(2).toObject().value("session_id").toString()};
    }

    static QJsonObject task(const QJsonObject& reply) { return reply.value("task").toObject(); }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void theListIsOneCompactRowPerTask() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const Members team = create(fixture);
        QVERIFY(!team.leader.isEmpty());
        const QString longText = QString(4000, QLatin1Char('d'));
        for (int index = 1; index <= 40; ++index) {
            const QJsonObject created = fixture.agents(
                team.leader, "agents.team_task_create",
                {{"title", QStringLiteral("Task number %1 with a readable title").arg(index)},
                 {"description", longText},
                 {"lane", "Web"},
                 {"criteria", QJsonArray{QString(300, QLatin1Char('c')), "Second"}},
                 {"depends_on", index == 2 ? QJsonArray{"T-1"} : QJsonArray{}}});
            QVERIFY2(created.value("ok").toBool(), qPrintable(text(created)));
        }
        const QString developerNumber = fixture.number(team.developer);
        QVERIFY(fixture
                    .agents(team.leader, "agents.team_task_assign",
                            {{"task_id", "T-1"}, {"owner", developerNumber}})
                    .value("ok")
                    .toBool());

        const QJsonObject listed = fixture.agents(team.developer, "agents.team_tasks");
        QVERIFY2(listed.value("ok").toBool(), qPrintable(text(listed)));
        const QJsonArray rows = listed.value("tasks").toArray();
        QCOMPARE(rows.size(), 40);
        QVERIFY2(QJsonDocument(listed).toJson(QJsonDocument::Compact).size() < 16 * 1024,
                 QByteArray::number(QJsonDocument(listed).toJson(QJsonDocument::Compact).size()));
        const QJsonObject first = rows.first().toObject();
        const QStringList keys = first.keys();
        QCOMPARE(QSet<QString>(keys.cbegin(), keys.cend()),
                 (QSet<QString>{"id", "title", "state", "round", "owner", "lane", "depends_on",
                                "pull_request"}));
        QCOMPARE(first.value("owner").toObject().value("number").toString(), developerNumber);
        QVERIFY(!first.value("owner").toObject().value("name").toString().isEmpty());
        QVERIFY(rows.at(1).toObject().value("owner").isNull());
        QCOMPARE(rows.at(1).toObject().value("depends_on").toArray(), QJsonArray{"T-1"});

        // Mine: the developer's own task; another developer has none.
        QJsonArray mine = fixture.agents(team.developer, "agents.team_tasks", {{"mine", true}})
                              .value("tasks")
                              .toArray();
        QCOMPARE(mine.size(), 1);
        QCOMPARE(mine.first().toObject().value("id").toString(), QStringLiteral("T-1"));
        QVERIFY(fixture.agents(team.second, "agents.team_tasks", {{"mine", true}})
                    .value("tasks")
                    .toArray()
                    .isEmpty());

        // A blocked task says why; open leaves out the done ones.
        QVERIFY(fixture
                    .agents(team.developer, "agents.team_task_update",
                            {{"task_id", "T-1"}, {"state", "in_progress"}})
                    .value("ok")
                    .toBool());
        const QJsonObject blocked =
            fixture.agents(team.developer, "agents.team_task_update",
                           {{"task_id", "T-1"}, {"state", "blocked"}, {"question", "Which API?"}});
        QVERIFY2(blocked.value("ok").toBool(), qPrintable(text(blocked)));
        const QJsonObject row =
            fixture.agents(team.developer, "agents.team_tasks", {{"mine", true}})
                .value("tasks")
                .toArray()
                .first()
                .toObject();
        QCOMPARE(row.value("state").toString(), QStringLiteral("blocked"));
        QCOMPARE(row.value("question").toString(), QStringLiteral("Which API?"));
        const QJsonObject closed = fixture.agents(
            team.leader, "agents.team_task_close",
            {{"task_id", "T-40"}, {"evidence", "abc1234def"}, {"note", "Not needed"}});
        QVERIFY2(closed.value("ok").toBool(), qPrintable(text(closed)));
        const QJsonArray open = fixture.agents(team.leader, "agents.team_tasks", {{"open", true}})
                                    .value("tasks")
                                    .toArray();
        QCOMPARE(open.size(), 39);
        for (const QJsonValue& value : open)
            QVERIFY(value.toObject().value("state").toString() != QStringLiteral("done"));
    }

    void oneTaskReadsItsHistoryPageByPage() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const Members team = create(fixture);
        QVERIFY(!team.leader.isEmpty());
        QVERIFY(fixture
                    .agents(team.leader, "agents.team_task_create",
                            {{"title", "Cart"},
                             {"description", "Context"},
                             {"criteria", QJsonArray{"Adds an item"}}})
                    .value("ok")
                    .toBool());
        QVERIFY(fixture
                    .agents(team.leader, "agents.team_task_assign",
                            {{"task_id", "T-1"}, {"owner", fixture.number(team.developer)}})
                    .value("ok")
                    .toBool());
        for (int index = 0; index < 25; ++index)
            QVERIFY(
                fixture
                    .agents(team.developer, "agents.team_task_update",
                            {{"task_id", "T-1"}, {"note", QStringLiteral("Note %1").arg(index)}})
                    .value("ok")
                    .toBool());

        const QJsonObject newest =
            task(fixture.agents(team.developer, "agents.team_tasks", {{"task_id", "T-1"}}));
        const int total = newest.value("history_total").toInt();
        QVERIFY(total >= 27);
        QCOMPARE(newest.value("description").toString(), QStringLiteral("Context"));
        QCOMPARE(newest.value("criteria").toArray().size(), 1);
        QJsonArray history = newest.value("history").toArray();
        QCOMPARE(history.size(), 10);
        QCOMPARE(newest.value("history_start").toInt(), total - 10);
        QCOMPARE(history.last().toObject().value("text").toString(), QStringLiteral("Note 24"));
        QCOMPARE(history.last().toObject().value("entry").toInt(), total - 1);

        // The first page, a middle one and the last one.
        const QJsonObject firstPage =
            task(fixture.agents(team.developer, "agents.team_tasks",
                                {{"task_id", "T-1"}, {"history_start", 0}, {"history_limit", 5}}));
        QCOMPARE(firstPage.value("history_start").toInt(), 0);
        QCOMPARE(firstPage.value("history").toArray().size(), 5);
        QCOMPARE(firstPage.value("history").toArray().first().toObject().value("kind").toString(),
                 QStringLiteral("created"));
        const QJsonObject middle =
            task(fixture.agents(team.developer, "agents.team_tasks",
                                {{"task_id", "T-1"}, {"history_start", 12}, {"history_limit", 4}}));
        QCOMPARE(middle.value("history_start").toInt(), 12);
        history = middle.value("history").toArray();
        QCOMPARE(history.size(), 4);
        for (int index = 0; index < 4; ++index)
            QCOMPARE(history.at(index).toObject().value("entry").toInt(), 12 + index);
        const QJsonObject last = task(fixture.agents(
            team.developer, "agents.team_tasks",
            {{"task_id", "T-1"}, {"history_start", total - 3}, {"history_limit", 10}}));
        QCOMPARE(last.value("history").toArray().size(), 3);
        QCOMPARE(last.value("history_total").toInt(), total);
        QCOMPARE(last.value("history").toArray().last().toObject().value("text").toString(),
                 QStringLiteral("Note 24"));
        // Past the end is an empty page, not a refusal; the limit is bounded.
        QVERIFY(task(fixture.agents(team.developer, "agents.team_tasks",
                                    {{"task_id", "T-1"}, {"history_start", total + 5}}))
                    .value("history")
                    .toArray()
                    .isEmpty());
        QCOMPARE(
            task(fixture.agents(team.developer, "agents.team_tasks",
                                {{"task_id", "T-1"}, {"history_start", 0}, {"history_limit", 500}}))
                .value("history")
                .toArray()
                .size(),
            std::min(total, 50));

        // The desktop's task read is unchanged: the newest 50 entries, no entry index.
        const QJsonObject desktop = fixture.host->handleCommand(
            request("team.task.get", {{"team_id", fixture.agents(team.leader, "agents.team_get")
                                                      .value("team")
                                                      .toObject()
                                                      .value("id")
                                                      .toString()},
                                      {"task_id", "T-1"}}),
            "desktop-test");
        const QJsonArray desktopHistory =
            desktop.value("task").toObject().value("history").toArray();
        QCOMPARE(desktopHistory.size(), std::min(total, 50));
        QVERIFY(!desktopHistory.first().toObject().contains("entry"));
    }

    void aLongNoteIsReadWholeByItsEntry() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const Members team = create(fixture);
        QVERIFY(!team.leader.isEmpty());
        QVERIFY(fixture
                    .agents(team.leader, "agents.team_task_create",
                            {{"title", "Cart"}, {"criteria", QJsonArray{"Adds an item"}}})
                    .value("ok")
                    .toBool());
        QVERIFY(fixture
                    .agents(team.leader, "agents.team_task_assign",
                            {{"task_id", "T-1"}, {"owner", fixture.number(team.developer)}})
                    .value("ok")
                    .toBool());
        QString note;
        for (int index = 0; note.size() < 9000; ++index)
            note += QStringLiteral("line %1 of the hand-over; ").arg(index);
        note += QStringLiteral("THE END");
        QVERIFY(fixture
                    .agents(team.developer, "agents.team_task_update",
                            {{"task_id", "T-1"}, {"note", note}})
                    .value("ok")
                    .toBool());

        const QJsonObject page =
            task(fixture.agents(team.developer, "agents.team_tasks", {{"task_id", "T-1"}}));
        const QJsonObject cut = page.value("history").toArray().last().toObject();
        const int entry = cut.value("entry").toInt();
        const QString shown = cut.value("text").toString();
        QVERIFY(shown.size() <= ketplus::daemon::teams::maximumHistoryText);
        QVERIFY2(shown.endsWith(
                     QStringLiteral("team_tasks task_id=T-1 entry=%1 reads it whole)").arg(entry)),
                 qPrintable(shown.right(120)));

        const QJsonObject whole = fixture.agents(team.developer, "agents.team_tasks",
                                                 {{"task_id", "T-1"}, {"entry", entry}});
        QVERIFY2(whole.value("ok").toBool(), qPrintable(text(whole)));
        QCOMPARE(task(whole).value("entry").toObject().value("text").toString(), note);
        QCOMPARE(task(whole).value("entry").toObject().value("entry").toInt(), entry);
        // A short entry reads as it is; an index outside the history is refused.
        QCOMPARE(task(fixture.agents(team.developer, "agents.team_tasks",
                                     {{"task_id", "T-1"}, {"entry", 0}}))
                     .value("entry")
                     .toObject()
                     .value("kind")
                     .toString(),
                 QStringLiteral("created"));
        QCOMPARE(fixture
                     .agents(team.developer, "agents.team_tasks",
                             {{"task_id", "T-1"}, {"entry", entry + 1}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("entry_not_found"));
    }
};

QTEST_GUILESS_MAIN(TeamAgentTasksTest)
#include "TeamAgentTasksTest.moc"
