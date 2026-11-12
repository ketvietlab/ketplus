// ADR 0114: the team_* tools per role, addressing by role and the role instructions, with
// the scripted fake providers. No real provider is started.
#include "AgentTestFixture.h"
#include "agents/AgentInstructions.h"
#include "agents/AgentMcpServer.h"

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

struct Team final {
    QString id;
    QString leader;
    QString developer;
    QString second;
    QString reviewer;
};

} // namespace

class TeamAgentToolsTest final : public QObject {
    Q_OBJECT

  private:
    QJsonObject command(Fixture& fixture, const QString& type, const QJsonObject& payload) {
        return fixture.host->handleCommand(request(type, payload), "desktop-test");
    }

    Team create(Fixture& fixture, const QString& goal = {}) {
        const QString repository = fixture.directory.filePath("shop");
        QDir().mkpath(repository);
        git(repository, {"init", "-q", "-b", "integration"});
        git(repository, {"-c", "user.email=t@example.com", "-c", "user.name=T", "commit", "-q",
                         "--allow-empty", "-m", "start"});
        const QJsonObject created = command(
            fixture, "team.create",
            {{"repository", repository},
             {"name", "Shop"},
             {"goal", goal},
             {"integration_branch", "integration"},
             {"plan",
              QJsonObject{
                  {"leader", QJsonObject{{"provider", "claude"}}},
                  {"developers",
                   QJsonObject{{"count", 2},
                               {"provider", "codex"},
                               {"members", QJsonArray{QJsonObject{{"paths", QJsonArray{"web"}}},
                                                      QJsonObject{{"paths", QJsonArray{"api"}}}}}}},
                  {"reviewer", QJsonObject{{"provider", "claude"}}}}}});
        const QJsonArray members = created.value("team").toObject().value("members").toArray();
        if (members.size() != 4)
            return {};
        return {.id = created.value("team").toObject().value("id").toString(),
                .leader = members.at(0).toObject().value("session_id").toString(),
                .developer = members.at(1).toObject().value("session_id").toString(),
                .second = members.at(2).toObject().value("session_id").toString(),
                .reviewer = members.at(3).toObject().value("session_id").toString()};
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void eachRoleMayDoOnlyItsRow() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const Team team = create(fixture);
        QVERIFY(!team.id.isEmpty());
        // Everyone reads the team.
        for (const QString& member : {team.leader, team.developer, team.reviewer}) {
            QVERIFY(fixture.agents(member, "agents.team_get").value("ok").toBool());
            QVERIFY(fixture.agents(member, "agents.team_tasks").value("ok").toBool());
        }
        QCOMPARE(fixture.agents(team.leader, "agents.team_get")
                     .value("you")
                     .toObject()
                     .value("role")
                     .toString(),
                 QStringLiteral("leader"));
        // The leader plans; developers and the reviewer may not.
        for (const QString& member : {team.developer, team.reviewer}) {
            const QJsonObject refused =
                fixture.agents(member, "agents.team_task_create", {{"title", "Mine"}});
            QCOMPARE(refused.value("error_code").toString(), QStringLiteral("role_forbidden"));
            QVERIFY(refused.value("error_message").toString().contains("leader"));
        }
        QJsonObject created = fixture.agents(
            team.leader, "agents.team_task_create",
            {{"title", "Cart"}, {"lane", "Web"}, {"criteria", QJsonArray{"Adds an item"}}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(text(created)));
        QCOMPARE(created.value("task").toObject().value("id").toString(), QStringLiteral("T-1"));
        const QString developerNumber = fixture.number(team.developer);
        QCOMPARE(fixture
                     .agents(team.developer, "agents.team_task_assign",
                             {{"task_id", "T-1"}, {"owner", developerNumber}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("role_forbidden"));
        QCOMPARE(fixture
                     .agents(team.developer, "agents.team_task_criteria",
                             {{"task_id", "T-1"}, {"criteria", QJsonArray{"Changed"}}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("role_forbidden"));
        QCOMPARE(fixture
                     .agents(team.developer, "agents.team_lane_set",
                             {{"developer", developerNumber},
                              {"paths", QJsonArray{"x"}},
                              {"reason", "mine"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("role_forbidden"));
        QCOMPARE(fixture.agents(team.developer, "agents.team_decision_raise", {{"question", "?"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("role_forbidden"));
        // Nothing changed.
        QJsonObject task =
            command(fixture, "team.task.get", {{"team_id", team.id}, {"task_id", "T-1"}})
                .value("task")
                .toObject();
        QVERIFY(task.value("owner").toString().isEmpty());
        QCOMPARE(task.value("criteria").toArray().first().toObject().value("text").toString(),
                 QStringLiteral("Adds an item"));
        QVERIFY(fixture
                    .agents(team.leader, "agents.team_task_assign",
                            {{"task_id", "T-1"}, {"owner", "#" + developerNumber}})
                    .value("ok")
                    .toBool());
        // The leader and the reviewer report on no task; another developer cannot either.
        QCOMPARE(
            fixture
                .agents(team.leader, "agents.team_task_update", {{"task_id", "T-1"}, {"note", "x"}})
                .value("error_code")
                .toString(),
            QStringLiteral("role_forbidden"));
        QCOMPARE(fixture
                     .agents(team.reviewer, "agents.team_task_update",
                             {{"task_id", "T-1"}, {"state", "in_progress"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("role_forbidden"));
        QCOMPARE(fixture
                     .agents(team.second, "agents.team_task_update",
                             {{"task_id", "T-1"}, {"state", "in_progress"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("not_owner"));
        QJsonObject updated =
            fixture.agents(team.developer, "agents.team_task_update",
                           {{"task_id", "T-1"},
                            {"state", "in_progress"},
                            {"note", "Started"},
                            {"met", QJsonArray{QJsonObject{{"index", 0}, {"met", true}}}}});
        QVERIFY2(updated.value("ok").toBool(), qPrintable(text(updated)));
        QCOMPARE(updated.value("task").toObject().value("state").toString(),
                 QStringLiteral("in_progress"));
        QCOMPARE(updated.value("task").toObject().value("criteria_met").toInt(), 1);
        // The leader changes a lane with a reason.
        QVERIFY(fixture
                    .agents(team.leader, "agents.team_lane_set",
                            {{"developer", developerNumber},
                             {"lane", "Web"},
                             {"paths", QJsonArray{"web", "docs"}},
                             {"reason", "docs too"}})
                    .value("ok")
                    .toBool());
        // T-19: only the leader voids a review round or accepts a review; T-1 has neither.
        for (const QString& member : {team.developer, team.reviewer})
            for (const char* operation : {"agents.team_review_void", "agents.team_review_accept"})
                QCOMPARE(fixture
                             .agents(member, operation,
                                     {{"task_id", "T-1"}, {"reason", "x"}, {"evidence", "y"}})
                             .value("error_code")
                             .toString(),
                         QStringLiteral("role_forbidden"));
        for (const char* operation : {"agents.team_review_void", "agents.team_review_accept"})
            QCOMPARE(fixture
                         .agents(team.leader, operation,
                                 {{"task_id", "T-1"}, {"reason", "x"}, {"evidence", "y"}})
                         .value("error_code")
                         .toString(),
                     QStringLiteral("invalid_transition"));
        // A conversation on no team has no team tools.
        const QString loner = fixture.start("codex", "alone");
        QCOMPARE(fixture.agents(loner, "agents.team_get").value("error_code").toString(),
                 QStringLiteral("not_on_team"));
    }

    // ADR 0114 §8: a member says what it waits for; a message it sent outside the team that
    // failed comes back to it as a turn.
    void aMemberWaitsAndHearsOfAFailedOutsideMessage() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const Team team = create(fixture);
        QVERIFY(!team.id.isEmpty());
        QCOMPARE(
            fixture.agents(team.reviewer, "agents.team_wait", {{"reason", "CI"}, {"minutes", 5}})
                .value("error_code")
                .toString(),
            QStringLiteral("role_forbidden"));
        QCOMPARE(fixture.agents(team.developer, "agents.team_wait", {{"reason", "CI"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("invalid_payload"));
        const QJsonObject waiting =
            fixture.agents(team.developer, "agents.team_wait", {{"reason", "CI"}, {"minutes", 5}});
        QVERIFY2(waiting.value("ok").toBool(), qPrintable(text(waiting)));
        QCOMPARE(waiting.value("wakes_in_minutes").toInt(), 5);

        // The outside conversation's turn cannot start, so no answer will ever arrive.
        const QString outside = fixture.start("claude", "outside");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(outside, "turn.completed"), 1, 5000);
        const auto failed = [&] {
            for (const AgentMessageRecord& message :
                 fixture.journal->agentMessagesOf(team.leader, 1, 20))
                if (message.kind == QStringLiteral("message_failed"))
                    return message.body;
            return QString{};
        };
        QVERIFY(failed().isEmpty());
        const QJsonObject sent = fixture.agents(team.leader, "agents.send",
                                                {{"to", "#" + fixture.number(outside)},
                                                 {"text", "fail-start"},
                                                 {"expect_reply", true}});
        QVERIFY2(sent.value("ok").toBool(), qPrintable(text(sent)));
        QTRY_VERIFY_WITH_TIMEOUT(!failed().isEmpty(), 5000);
        QVERIFY(failed().contains("#" + fixture.number(outside)));
        const auto request = fixture.journal->agentMessage(sent.value("message_id").toString());
        QCOMPARE(request->state, QStringLiteral("failed"));
        QCOMPARE(request->error, QStringLiteral("turn_failed"));
    }

    // T-23: a second reviewer is added in the settings; "reviewer" then names the one of the
    // sender's work, and a sender with no such work is told to use a number.
    void twoReviewersAreAddressedByTheirWork() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const Team team = create(fixture);
        QVERIFY(!team.id.isEmpty());
        QCOMPARE(command(fixture, "team.member.add",
                         {{"team_id", team.id}, {"role", "reviewer"}, {"provider", "codex"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("reviewer_unsupported"));
        const QJsonObject added =
            command(fixture, "team.member.add",
                    {{"team_id", team.id}, {"role", "reviewer"}, {"provider", "claude"}});
        QVERIFY2(added.value("ok").toBool(), qPrintable(text(added)));
        const QJsonArray members = added.value("team").toObject().value("members").toArray();
        QCOMPARE(members.size(), 5);
        const QJsonObject second = members.last().toObject();
        QCOMPARE(second.value("role").toString(), QStringLiteral("reviewer"));
        QCOMPARE(second.value("name").toString(), QStringLiteral("Reviewer 2"));
        const QString secondReviewer = second.value("session_id").toString();
        QCOMPARE(command(fixture, "team.member.add",
                         {{"team_id", team.id}, {"role", "reviewer"}, {"provider", "claude"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("role_taken"));
        // The team's description lists both.
        const QJsonArray reviewers = fixture.agents(team.developer, "agents.whoami")
                                         .value("team")
                                         .toObject()
                                         .value("reviewers")
                                         .toArray();
        QCOMPARE(reviewers.size(), 2);
        QCOMPARE(reviewers.at(1).toObject().value("number").toString(),
                 fixture.number(secondReviewer));
        // No task of the developer is with either: it is told to use a number.
        const QJsonObject unclear = fixture.agents(team.developer, "agents.send",
                                                   {{"to", "reviewer"}, {"text", "Which one?"}});
        QCOMPARE(unclear.value("error_code").toString(), QStringLiteral("ambiguous_role"));
        const QString message = unclear.value("error_message").toString();
        QVERIFY2(message.contains("#" + fixture.number(team.reviewer)) &&
                     message.contains("#" + fixture.number(secondReviewer)),
                 qPrintable(text(unclear)));
        // Removing one of two is allowed, the last one is not.
        QVERIFY(command(fixture, "team.member.remove",
                        {{"team_id", team.id}, {"session_id", secondReviewer}})
                    .value("ok")
                    .toBool());
        QCOMPARE(command(fixture, "team.member.remove",
                         {{"team_id", team.id}, {"session_id", team.reviewer}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("last_reviewer"));
    }

    void rolesAreAddressesInsideATeam() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const Team team = create(fixture);
        QVERIFY(!team.id.isEmpty());
        const QJsonObject sent = fixture.agents(
            team.developer, "agents.send", {{"to", "reviewer"}, {"text", "Ready for a look?"}});
        QVERIFY2(sent.value("ok").toBool(), qPrintable(text(sent)));
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.journal->agentMessagesOf(team.reviewer, 1, 5).isEmpty(),
                                 5000);
        QCOMPARE(fixture.journal->agentMessagesOf(team.reviewer, 1, 5).first().toSession,
                 team.reviewer);
        QVERIFY(fixture.agents(team.reviewer, "agents.send", {{"to", "Leader"}, {"text", "Hi"}})
                    .value("ok")
                    .toBool());
        const QString loner = fixture.start("codex", "alone");
        const QJsonObject refused =
            fixture.agents(loner, "agents.send", {{"to", "reviewer"}, {"text", "Hello"}});
        QCOMPARE(refused.value("error_code").toString(), QStringLiteral("not_on_team"));
        // agents_list and agents_whoami show the role and lane.
        const QJsonObject whoami = fixture.agents(team.developer, "agents.whoami");
        QCOMPARE(whoami.value("team").toObject().value("role").toString(),
                 QStringLiteral("developer"));
        QCOMPARE(whoami.value("team").toObject().value("reviewer_number").toString(),
                 fixture.number(team.reviewer));
        bool listed = false;
        for (const auto& value :
             fixture.agents(loner, "agents.list").value("conversations").toArray())
            if (value.toObject().value("number").toString() == fixture.number(team.developer)) {
                listed = value.toObject().value("role").toString() == "developer" &&
                         value.toObject().value("lane").toString() == "Developer 1";
            }
        QVERIFY(listed);
        // Leaving the team removes the role and the tools.
        QVERIFY(command(fixture, "team.member.remove",
                        {{"team_id", team.id}, {"session_id", team.second}})
                    .value("ok")
                    .toBool());
        QVERIFY(!fixture.agents(team.second, "agents.whoami").contains("team"));
        QCOMPARE(fixture.agents(team.second, "agents.team_get").value("error_code").toString(),
                 QStringLiteral("not_on_team"));
    }

    void instructionsFollowTheRole() {
        using agent_messaging::teamInstructions;
        const QString leader = teamInstructions({.team = "Shop",
                                                 .goal = "Ship",
                                                 .role = "leader",
                                                 .mergePolicy = "leader",
                                                 .integrationBranch = "integration"});
        QVERIFY(leader.contains("Do not write code and do not review"));
        QVERIFY(leader.contains("Never approve a permission request"));
        QVERIFY(leader.contains("You merge each Ready task into integration"));
        QVERIFY(
            teamInstructions({.role = "leader", .mergePolicy = "user", .integrationBranch = "i"})
                .contains("The user merges reviewed tasks"));
        const QString developer = teamInstructions(
            {.team = "Shop", .role = "developer", .lane = "Web", .paths = {"web"}});
        QVERIFY(developer.contains("Your lane is \"Web\": web."));
        QVERIFY(developer.contains("Report through the task"));
        QVERIFY(developer.contains("Do not merge."));
        const QString reviewer = teamInstructions({.team = "Shop", .role = "reviewer"});
        QVERIFY(reviewer.contains("Verdict: no findings"));
        QVERIFY(reviewer.contains("read-only"));
    }

    void theLeadersFirstTurnCarriesTheGoalAndItsInstructions() {
        Fixture fixture;
        QVERIFY(fixture.open(true));
        const Team team = create(fixture, "Ship the cart");
        QVERIFY(!team.id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(team.leader, "turn.completed"), 1, 8000);
        const QByteArray log = logText(fixture.claudeLog);
        QVERIFY(log.contains("[KetPlus team]"));
        QVERIFY(log.contains("You are the leader of team"));
        QVERIFY(log.contains("Ship the cart"));
        // Developers and the reviewer wait for work.
        QCOMPARE(fixture.count(team.developer, "user.message"), 0);
        QCOMPARE(fixture.count(team.reviewer, "user.message"), 0);
    }

    void toolsAreListedOnlyForAMember() {
        const auto listWith = [](const QJsonObject& whoami) {
            const QJsonObject answer = agent_mcp::handleMessage(
                {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
                [&](const QString& operation, const QJsonObject&) {
                    return operation == "agents.whoami" ? whoami : QJsonObject{};
                });
            QStringList names;
            for (const auto& tool : answer.value("result").toObject().value("tools").toArray())
                names.append(tool.toObject().value("name").toString());
            return names;
        };
        const QStringList member =
            listWith({{"ok", true}, {"team", QJsonObject{{"role", "leader"}}}});
        QVERIFY(member.contains("team_task_create"));
        QVERIFY(member.contains("team_decision_raise"));
        QVERIFY(member.contains("team_task_close"));
        QVERIFY(member.contains("team_review_void"));
        QVERIFY(member.contains("team_review_accept"));
        const QStringList stranger = listWith({{"ok", true}, {"number", "1234"}});
        QVERIFY(!stranger.contains("team_get"));
        QVERIFY(stranger.contains("agents_send"));
    }
};

QTEST_GUILESS_MAIN(TeamAgentToolsTest)
#include "TeamAgentToolsTest.moc"
