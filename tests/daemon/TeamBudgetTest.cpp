// ADR 0114, open point 2: the automatic-turn budget. A global value the daemon keeps, which a
// team takes when it is created without its own, and each team's own value, which the user
// changes while it runs. The coordinator cases use a fake runtime; the protocol cases the
// scripted fake providers. No real provider is started.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "protocol/DaemonProtocol.h"

#include <QStandardPaths>
#include <QTest>

using namespace ketplus::daemon;
using namespace team_test;

namespace {

QJsonObject plan() {
    return {{"leader", QJsonObject{{"provider", "claude"}}},
            {"developers",
             QJsonObject{{"count", 1},
                         {"provider", "codex"},
                         {"members", QJsonArray{QJsonObject{{"paths", QJsonArray{"web"}}}}}}},
            {"reviewer", QJsonObject{{"provider", "claude"}}}};
}

// Out of range, not whole or not a number: each is refused in words.
const QList<QJsonValue>& badBudgets() {
    static const QList<QJsonValue> values{QJsonValue(4),    QJsonValue(201),
                                          QJsonValue(0),    QJsonValue(-3),
                                          QJsonValue(7.5),  QJsonValue(QStringLiteral("40")),
                                          QJsonValue(true), QJsonValue(QJsonValue::Null)};
    return values;
}

const QString rangeMessage = QStringLiteral("Automatic turns are a whole number from 5 to 200.");

} // namespace

class TeamBudgetTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    void start(const int budget) {
        Team draft = standardTeam();
        draft.turnBudget = budget;
        team_ = teams_->create(draft, "user").teamId;
        QVERIFY(team_ > 0);
    }
    QString task(const QString& title) {
        QString id;
        (void)teams_->createTask(team_, {.title = title, .lane = "Web"}, "lead", &id);
        return id;
    }
    const Team& team() { return *teams_->team(team_); }
    int openBudgetDecisions() {
        int open = 0;
        for (const Decision& decision : team().decisions)
            open += decision.kind == "budget" && decision.state == "open" ? 1 : 0;
        return open;
    }

    // Two automatic turns of a budget of 2: the third assignment is held and the team paused.
    void useTheBudget() {
        start(2);
        QStringList ids;
        for (int index = 0; index < 3; ++index)
            ids << task(QStringLiteral("Task %1").arg(index));
        QVERIFY(teams_->assignTask(team_, ids.at(0), "dev1", "lead").ok());
        QVERIFY(teams_->assignTask(team_, ids.at(1), "dev2", "lead").ok());
        QVERIFY(teams_->assignTask(team_, ids.at(2), "dev1", "lead").ok());
        QVERIFY(team().paused);
        QCOMPARE(team().pausedReason, QStringLiteral("budget"));
        QCOMPARE(team().automaticTurns, 2);
        QCOMPARE(openBudgetDecisions(), 1);
        QCOMPARE(host_.of("assignment").size(), 2);
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        teams_.reset();
        journal_.reset();
        QFile::remove(directory_.filePath("journal.sqlite3"));
        host_ = {};
        for (const QString& id : {"lead", "dev1", "dev2"})
            host_.add(id);
        host_.add("rev", "claude");
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(60 * 60 * 1000);
        teams_->load();
    }

    // A budget that leaves turns withdraws the open question and resumes the team; the turns
    // used stay counted, and the "Continue" choice then offers the team's new budget.
    void aLargerBudgetWithdrawsTheQuestionAndResumes() {
        useTheBudget();
        const QString question = team().decisions.last().id;
        const auto result = teams_->update(team_, {{"turn_budget", 5}}, "user");
        QVERIFY2(result.ok(), qPrintable(result.error.value_or(TeamError{}).message));
        QVERIFY(!team().paused);
        QCOMPARE(team().turnBudget, 5);
        QCOMPARE(openBudgetDecisions(), 0);
        QCOMPARE(team().decision(question)->state, QStringLiteral("withdrawn"));
        QCOMPARE(team().decision(question)->reason,
                 QStringLiteral("The budget is now 5 automatic turns."));
        // The two turns used stay counted: the held assignment went out as the third of five.
        QCOMPARE(host_.of("assignment").size(), 3);
        QCOMPARE(team().automaticTurns, 3);
        // Two more turns, then the question again, now with the team's own budget.
        QStringList ids{task("Four"), task("Five"), task("Six")};
        for (const QString& id : ids)
            QVERIFY(teams_->assignTask(team_, id, "dev2", "lead").ok());
        QVERIFY(team().paused);
        QCOMPARE(team().automaticTurns, 5);
        QCOMPARE(team().decisions.last().question,
                 QStringLiteral("The team used its 5 automatic turns. Continue?"));
        QVERIFY(team().decisions.last().choices.contains("Continue (5 more turns)"));
    }

    // A team the user pauses in the same change stays paused: nothing held goes out.
    void aPauseByTheUserOutlastsANewBudget() {
        useTheBudget();
        QVERIFY(teams_->update(team_, {{"turn_budget", 5}, {"paused", true}}, "user").ok());
        QVERIFY(team().paused);
        QCOMPARE(team().pausedReason, QStringLiteral("user"));
        QCOMPARE(team().turnBudget, 5);
        QCOMPARE(host_.of("assignment").size(), 2);
    }

    // The value is checked before anything changes; the team is as it was.
    void anInvalidTeamBudgetIsRefusedInWords() {
        start(40);
        for (const QJsonValue& bad : badBudgets()) {
            const auto refused =
                teams_->update(team_, {{"turn_budget", bad}, {"paused", true}}, "user");
            QCOMPARE(refused.error.value_or(TeamError{}).code, QStringLiteral("invalid_payload"));
            QCOMPARE(refused.error.value_or(TeamError{}).message, rangeMessage);
            QCOMPARE(team().turnBudget, 40);
            QVERIFY(!team().paused);
        }
        for (const int edge : {5, 200})
            QVERIFY(teams_->update(team_, {{"turn_budget", edge}}, "user").ok());
        QCOMPARE(team().turnBudget, 200);
    }

    // The global value: 40 until changed, kept by the daemon, refused out of range, and the
    // value a team created without its own takes. Changing it changes no existing team.
    void theGlobalValueIsWhatANewTeamStartsFrom() {
        agent_test::Fixture fixture;
        QVERIFY(fixture.open());
        const auto command = [&](const QString& type, const QJsonObject& payload) {
            return fixture.host->handleCommand(agent_test::request(type, payload), "desktop-test");
        };
        const auto create = [&](const QString& name, const QJsonObject& extra = {}) {
            QJsonObject payload{{"repository", fixture.directory.path()},
                                {"name", name},
                                {"integration_branch", "integration"},
                                {"merge_policy", "leader"},
                                {"plan", plan()}};
            for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
                payload.insert(it.key(), it.value());
            return command("team.create", payload);
        };
        const auto budgetOf = [&](const QJsonObject& created) {
            const QJsonValue id = created.value("team").toObject().value("id");
            return command("team.get", {{"team_id", id}})
                .value("team")
                .toObject()
                .value("turn_budget")
                .toInt();
        };
        QVERIFY(protocol::features(1).contains("teams.settings/1"));
        const QJsonObject initial = command("team.settings.get", {});
        QVERIFY(initial.value("ok").toBool());
        QCOMPARE(initial.value("settings").toObject(), (QJsonObject{{"turn_budget", 40},
                                                                    {"turn_budget_minimum", 5},
                                                                    {"turn_budget_maximum", 200},
                                                                    {"turn_budget_default", 40}}));
        for (const QJsonValue& bad : badBudgets()) {
            const QJsonObject refused = command("team.settings.update", {{"turn_budget", bad}});
            QCOMPARE(refused.value("error_code").toString(), QStringLiteral("invalid_payload"));
            QCOMPARE(refused.value("error_message").toString(), rangeMessage);
            const QJsonObject made = create("Refused", {{"turn_budget", bad}});
            QCOMPARE(made.value("error_code").toString(), QStringLiteral("invalid_payload"));
            QCOMPARE(made.value("error_message").toString(), rangeMessage);
        }
        QCOMPARE(
            command("team.settings.update", {{"colour", "red"}}).value("error_code").toString(),
            QStringLiteral("invalid_payload"));
        QCOMPARE(command("team.list", {}).value("teams").toArray().size(), 0);

        const QJsonObject first = create("First");
        QVERIFY2(first.value("ok").toBool(), qPrintable(first.value("error_message").toString()));
        QCOMPARE(budgetOf(first), 40);
        const QJsonObject changed = command("team.settings.update", {{"turn_budget", 60}});
        QVERIFY2(changed.value("ok").toBool(),
                 qPrintable(changed.value("error_message").toString()));
        QCOMPARE(changed.value("settings").toObject().value("turn_budget").toInt(), 60);
        QCOMPARE(command("team.settings.get", {})
                     .value("settings")
                     .toObject()
                     .value("turn_budget")
                     .toInt(),
                 60);
        const QJsonObject second = create("Second");
        const QJsonObject third = create("Third", {{"turn_budget", 12}});
        QCOMPARE(budgetOf(second), 60);
        QCOMPARE(budgetOf(third), 12);
        QVERIFY(command("team.settings.update", {{"turn_budget", 80}}).value("ok").toBool());
        QCOMPARE(budgetOf(first), 40);
        QCOMPARE(budgetOf(second), 60);
        QCOMPARE(budgetOf(third), 12);
        // The team's own value over the protocol, in range only.
        const QJsonValue id = second.value("team").toObject().value("id");
        QCOMPARE(command("team.update", {{"team_id", id}, {"turn_budget", 300}})
                     .value("error_message")
                     .toString(),
                 rangeMessage);
        QCOMPARE(command("team.update", {{"team_id", id}, {"turn_budget", 25}})
                     .value("team")
                     .toObject()
                     .value("turn_budget")
                     .toInt(),
                 25);
    }

    // The global value is the daemon's, not a client's: a new coordinator reads it back.
    void theGlobalValueOutlivesTheDaemon() {
        QVERIFY(!teams_->updateTeamSettings({{"turn_budget", 75}}));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->load();
        QCOMPARE(teams_->turnBudgetSetting(), 75);
        // A value outside the range in the journal (written by hand) counts as none.
        QVERIFY(journal_->saveTeamSettings({{"turn_budget", 9000}}));
        QCOMPARE(teams_->turnBudgetSetting(), 40);
    }
};

QTEST_GUILESS_MAIN(TeamBudgetTest)
#include "TeamBudgetTest.moc"
