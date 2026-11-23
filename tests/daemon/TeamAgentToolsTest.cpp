// ADR 0114: the team_* tools per role, addressing by role and the role instructions, with
// the scripted fake providers. No real provider is started.
#include "AgentTestFixture.h"
#include "TestPaths.h"
#include "agents/AgentBroker.h"
#include "agents/AgentInstructions.h"
#include "agents/AgentMcpServer.h"
#include "teams/TeamRules.h"

#include <QProcess>
#include <QScopeGuard>
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
    // The answer of the last create(), for a refusal.
    QJsonObject lastCreated_;

    QJsonObject command(Fixture& fixture, const QString& type, const QJsonObject& payload) {
        return fixture.host->handleCommand(request(type, payload), "desktop-test");
    }

    Team create(Fixture& fixture, const QString& goal = {},
                const QString& name = QStringLiteral("Shop"),
                const QString& provider = QStringLiteral("claude")) {
        const QString repository = fixture.directory.filePath(name.toLower());
        QDir().mkpath(repository);
        git(repository, {"init", "-q", "-b", "integration"});
        git(repository, {"-c", "user.email=t@example.com", "-c", "user.name=T", "commit", "-q",
                         "--allow-empty", "-m", "start"});
        const QJsonObject created = command(
            fixture, "team.create",
            {{"repository", repository},
             {"name", name},
             {"goal", goal},
             {"integration_branch", "integration"},
             {"plan",
              QJsonObject{{"leader", QJsonObject{{"provider", provider}}},
                          {"developers",
                           QJsonObject{{"count", 2},
                                       {"provider", "codex"},
                                       {"members", QJsonArray{QJsonObject{{"lane", "Web"}},
                                                              QJsonObject{{"lane", "Api"}}}}}},
                          {"reviewer", QJsonObject{{"provider", "claude"}}}}}});
        lastCreated_ = created;
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
                             {{"developer", developerNumber}, {"lane", "x"}, {"reason", "mine"}})
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
        // The leader renames a lane with a reason.
        QVERIFY(fixture
                    .agents(team.leader, "agents.team_lane_set",
                            {{"developer", developerNumber},
                             {"lane", "Storefront"},
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
                         value.toObject().value("lane").toString() == "Web";
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
        using agent_messaging::conversationContext;
        using agent_messaging::teamInstructions;
        const QString leader = teamInstructions("leader");
        QVERIFY(leader.contains("Answer explanation, advice, synthesis and coordination directly"));
        QVERIFY(leader.contains("developer_leader"));
        QVERIFY(leader.contains("developer_reviewer"));
        QVERIFY(leader.contains("Confidence above 90 is only supplementary"));
        QVERIFY(!leader.contains("reviewer reviews each task"));
        QVERIFY(leader.contains("Never approve a permission request"));
        QVERIFY(conversationContext("79461", {.role = "leader", .integrationBranch = "integration"})
                    .contains("You merge each Ready task into integration"));
        QVERIFY(conversationContext(
                    "79461", {.role = "leader", .mergePolicy = "user", .integrationBranch = "i"})
                    .contains("The user merges reviewed tasks"));
        const QString developer = teamInstructions("developer");
        QVERIFY(conversationContext("79461", {.role = "developer", .lane = "Web"})
                    .contains("Your lane is \"Web\"."));
        QVERIFY(!developer.contains("paths"));
        QVERIFY(!developer.contains("outside the lane"));
        QVERIFY(!leader.contains("team_paths_allow"));
        QVERIFY(!leader.contains("paths"));
        QVERIFY(developer.contains("Call team_get"));
        QVERIFY(developer.contains("Report through the task"));
        QVERIFY(developer.contains("Do not merge."));
        QVERIFY(developer.contains("submit evidence once"));
        QVERIFY(developer.contains("chosen route requires review"));
        QVERIFY(
            leader.contains("Do not ask the user to approve again an action already authorized"));
        const QString reviewer = teamInstructions("reviewer");
        QVERIFY(reviewer.contains("Call team_get"));
        QVERIFY(reviewer.contains("Verdict: no findings"));
        QVERIFY(reviewer.contains("read-only"));
    }

    void memberIdentityDoesNotChangeHostPrefix_data() {
        QTest::addColumn<QString>("provider");
        for (const auto* provider : {"claude", "codex", "cursor"})
            QTest::newRow(provider) << QString::fromLatin1(provider);
    }

    void memberIdentityDoesNotChangeHostPrefix() {
        QFETCH(QString, provider);
        const auto oldInputLog = qgetenv("KETPLUS_FAKE_CODEX_LOG_INPUT");
        qputenv("KETPLUS_FAKE_CODEX_LOG_INPUT", "1");
        const auto restoreInputLog = qScopeGuard([oldInputLog] {
            if (oldInputLog.isNull())
                qunsetenv("KETPLUS_FAKE_CODEX_LOG_INPUT");
            else
                qputenv("KETPLUS_FAKE_CODEX_LOG_INPUT", oldInputLog);
        });
        Fixture fixture;
        QVERIFY(fixture.open(true));
        const QString log = provider == "claude"  ? fixture.claudeLog
                            : provider == "codex" ? fixture.codexLog
                                                  : fixture.cursorLog;
        const auto rows = [&](const QByteArray& prefix) {
            QList<QJsonObject> matches;
            for (const auto& line : logText(log).split('\n'))
                if (line.startsWith(prefix))
                    matches.append(QJsonDocument::fromJson(line.mid(prefix.size())).object());
            return matches;
        };
        const Team first = create(fixture, "Ship the cart", "Shop", provider);
        QVERIFY(!first.id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(first.leader, "turn.completed"), 1, 8000);
        const Team second = create(fixture, "Count stock", "Warehouse", provider);
        QVERIFY(!second.id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(second.leader, "turn.completed"), 1, 8000);
        const QString firstNumber = fixture.number(first.leader);
        const QString secondNumber = fixture.number(second.leader);
        QVERIFY(!firstNumber.isEmpty());
        QVERIFY(!secondNumber.isEmpty());
        QVERIFY(firstNumber != secondNumber);

        const bool codex = provider == "codex";
        const auto policy = rows(codex ? "setup:" : "policy:");
        QCOMPARE(policy.size(), 2);
        const QString instructionKey = codex ? "developerInstructions" : "instructions";
        const QByteArray instructions = policy.first().value(instructionKey).toString().toUtf8();
        QVERIFY(instructions.contains("Your team role is leader."));
        QVERIFY(instructions.contains("Call team_get"));
        QCOMPARE(instructions, policy.last().value(instructionKey).toString().toUtf8());

        // Compare the server declarations the fake provider received, excluding launch-only env.
        const auto setups = rows(provider == "cursor" ? "browser:" : "setup:");
        QCOMPARE(setups.size(), 2);
        const auto servers = [&](const QJsonObject& setup) {
            QJsonObject declarations;
            QJsonArray acpServers;
            if (codex) {
                const auto config = setup.value("config").toObject();
                for (auto it = config.begin(); it != config.end(); ++it)
                    if (it.key().startsWith("mcp_servers.") && !it.key().endsWith(".env"))
                        declarations.insert(it.key(), it.value());
            } else if (provider == "claude") {
                const auto mcp = setup.value("mcpServers").toObject();
                for (auto it = mcp.begin(); it != mcp.end(); ++it) {
                    auto server = it.value().toObject();
                    server.remove("env");
                    declarations.insert(it.key(), server);
                }
            } else {
                for (const auto& value : setup.value("mcpServers").toArray()) {
                    auto server = value.toObject();
                    server.remove("env");
                    acpServers.append(server);
                }
                declarations.insert("servers", acpServers);
            }
            return QJsonDocument(declarations).toJson(QJsonDocument::Compact);
        };
        const QByteArray serverBytes = servers(setups.first());
        QVERIFY(serverBytes.contains("ketplus_agents"));
        QCOMPARE(serverBytes, servers(setups.last()));

        const auto listedTools = [&](const QString& session) {
            return QJsonDocument(agent_mcp::handleMessage(
                                     {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
                                     [&](const QString& method, const QJsonObject& params) {
                                         return fixture.agents(session, method, params);
                                     }))
                .toJson(QJsonDocument::Compact);
        };
        const auto firstIdentity = fixture.agents(first.leader, "agents.whoami");
        const auto secondIdentity = fixture.agents(second.leader, "agents.whoami");
        QCOMPARE(firstIdentity.value("number").toString(), firstNumber);
        QCOMPARE(secondIdentity.value("number").toString(), secondNumber);
        QCOMPARE(firstIdentity.value("team").toObject().value("name").toString(), "Shop");
        QCOMPARE(secondIdentity.value("team").toObject().value("name").toString(), "Warehouse");
        QCOMPARE(firstIdentity.value("worktree").toString(), fixture.directory.filePath("shop"));
        QCOMPARE(secondIdentity.value("worktree").toString(),
                 fixture.directory.filePath("warehouse"));
        const QByteArray tools = listedTools(first.leader);
        QVERIFY(tools.contains("team_get"));
        QCOMPARE(tools, listedTools(second.leader));
        for (const auto& bytes : {instructions, serverBytes, tools}) {
            QVERIFY(!bytes.contains("Ship the cart"));
            QVERIFY(!bytes.contains("Count stock"));
            QVERIFY(!bytes.contains(fixture.directory.path().toUtf8()));
            QVERIFY(!bytes.contains(("@agent/" + firstNumber).toUtf8()));
            QVERIFY(!bytes.contains(("@agent/" + secondNumber).toUtf8()));
        }
        QList<QByteArray> inputs;
        for (const auto& line : logText(log).split('\n'))
            if (line.startsWith(codex ? "input:" : "policy:"))
                inputs.append(line);
        QCOMPARE(inputs.size(), 2);
        QVERIFY(inputs.first().contains("Ship the cart"));
        QVERIFY(inputs.last().contains("Count stock"));
        QVERIFY(inputs.first().contains(("@agent/" + firstNumber).toUtf8()));
        QVERIFY(inputs.last().contains(("@agent/" + secondNumber).toUtf8()));
    }

    void routingPromptFixturesStayConservative() {
        QFile file(QFINDTESTDATA("fixtures/team-routing-prompts-v1.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto fixtures = QJsonDocument::fromJson(file.readAll()).array();
        QCOMPARE(fixtures.size(), 3);
        const QString leader = agent_messaging::teamInstructions("leader");
        for (const auto& value : fixtures) {
            const auto scenario = value.toObject();
            QVERIFY(!scenario.value("synthetic_prompt").toString().isEmpty());
            QVERIFY(leader.contains(scenario.value("route").toString()));
            QVERIFY2(leader.contains(scenario.value("required_guidance").toString()),
                     qPrintable(scenario.value("name").toString()));
            QVERIFY(!leader.contains("reviewer reviews each task"));
            QVERIFY(!leader.contains("quota-consuming live tests"));
        }
    }

    void theLeadersFirstTurnCarriesTheGoalAndItsInstructions() {
        Fixture fixture;
        QVERIFY(fixture.open(true));
        const Team team = create(fixture, "Ship the cart");
        QVERIFY(!team.id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(team.leader, "turn.completed"), 1, 8000);
        const QByteArray log = logText(fixture.claudeLog);
        QJsonObject policy;
        for (const auto& line : log.split('\n')) {
            if (line.startsWith("policy:")) {
                policy = QJsonDocument::fromJson(line.mid(7)).object();
                break;
            }
        }
        const QString instructions = policy.value("instructions").toString();
        QVERIFY(instructions.contains("[KetPlus team]"));
        QVERIFY(instructions.contains("Your team role is leader."));
        QVERIFY(!instructions.contains("Ship the cart"));
        QVERIFY(!instructions.contains("This conversation is #"));
        const QString prompt = policy.value("prompt").toString();
        QVERIFY(prompt.contains("[KetPlus conversation context]"));
        QVERIFY(prompt.contains("You are the leader of team"));
        QVERIFY(prompt.contains("Ship the cart"));
        // Developers and the reviewer wait for work.
        QCOMPARE(fixture.count(team.developer, "user.message"), 0);
        QCOMPARE(fixture.count(team.reviewer, "user.message"), 0);
    }

    // ADR 0114 §9: the longest goal of multi-byte text reaches the leader through the broker
    // whole, in one message: nothing is shortened.
    void aLongNonAsciiGoalReachesTheLeaderThroughTheBrokerWhole() {
        Fixture fixture;
        QVERIFY(fixture.open(true));
        const QString phrase = QStringLiteral("Thuế hoá đơn — giỏ hàng ✓ đồng bộ ✓ 購物 ");
        QString goal = QStringLiteral("Hỏi @agent/8213 trước.\n");
        while (goal.toUtf8().size() + phrase.toUtf8().size() <= teams::rules::maximumGoalBytes)
            goal += phrase;
        QVERIFY(goal.size() < teams::rules::maximumGoalCharacters);
        const Team team = create(fixture, goal);
        QVERIFY(!team.id.isEmpty());
        QString body;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (const auto& message : fixture.journal->agentMessagesOf(team.leader, 1, 20))
                    if (message.kind == QStringLiteral("team"))
                        body = message.body;
                return !body.isEmpty();
            }(),
            8000);
        QVERIFY(body.contains(QStringLiteral("Goal: ") + goal.trimmed() + QLatin1Char('\n')));
        QVERIFY(!body.contains(QStringLiteral("(shortened)")));
        QVERIFY(body.toUtf8().size() <= AgentBroker::maximumBodyBytes);

        // Fewer than 16,000 characters but more than one message holds: refused, not cut.
        const QString over(12000, QChar(0x8CFC)); // 購, three bytes in UTF-8
        QVERIFY(over.toUtf8().size() > AgentBroker::maximumBodyBytes);
        QVERIFY(create(fixture, over).id.isEmpty());
        const QJsonObject refused = lastCreated_;
        QCOMPARE(refused.value("error_code").toString(), QStringLiteral("invalid_payload"));
        QVERIFY(refused.value("error_message").toString().contains("bytes"));
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
