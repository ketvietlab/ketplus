// ADR 0114 §5 and open point 3: who merges a reviewed task, the dependency order and the
// ancestor check. The coordinator cases use a fake runtime; the end-to-end case a temporary
// Git repository and the scripted fake providers. No provider quota, no hosting service.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"

#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>

using namespace ketplus::daemon;
using namespace team_test;

namespace {

void git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git",
                  QStringList{"-c", "user.email=t@example.com", "-c", "user.name=T"} + arguments);
    QVERIFY(process.waitForFinished(15000));
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
}

} // namespace

class TeamMergeTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    QString task(const QString& title, const QStringList& depends = {}) {
        QString id;
        (void)teams_->createTask(team_, {.title = title, .lane = "Web", .dependsOn = depends},
                                 "lead", &id);
        return id;
    }
    // Assigned to `owner`, reviewed and passed.
    void pass(const QString& id, const QString& owner) {
        QVERIFY(teams_->assignTask(team_, id, owner, "lead").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, owner).ok());
        QVERIFY(teams_->moveTask(team_, id, state::inReview, owner).ok());
        QCOMPARE(teams_->team(team_)->reviewing, id);
        finishReview(*teams_, "rev", "Verdict: no findings", true);
        QCOMPARE(get(id).state, state::ready);
    }
    const Team& team() { return *teams_->team(team_); }
    const Task& get(const QString& id) { return *team().task(id); }
    QList<const Decision*> merges() {
        QList<const Decision*> result;
        for (const Decision& decision : team().decisions)
            if (decision.kind == "merge")
                result.append(&decision);
        return result;
    }
    int updates(const QString& kind) {
        int count = 0;
        for (const TeamUpdateRecord& update : journal_->teamUpdates(team_, false, 300))
            count += update.kind == kind ? 1 : 0;
        return count;
    }

    struct Ready {
        QString teamId;
        QString repository;
        QString worktree;
        QString branch;
    };
    static QJsonObject readTeam(agent_test::Fixture& fixture, const QString& teamId) {
        return fixture.host
            ->handleCommand(agent_test::request("team.get", {{"team_id", teamId}}), "desktop-test")
            .value("team")
            .toObject();
    }
    static QString stateOf(agent_test::Fixture& fixture, const QString& teamId) {
        return readTeam(fixture, teamId)
            .value("tasks")
            .toArray()
            .first()
            .toObject()
            .value("state")
            .toString();
    }
    // A real repository with one task committed, reviewed with no findings and ready.
    void readyOnARealRepository(agent_test::Fixture& fixture, Ready& ready) {
        QVERIFY(fixture.open(true));
        const QString repository = fixture.directory.filePath("shop");
        QVERIFY(QDir().mkpath(repository));
        git(repository, {"init", "-q", "-b", "integration"});
        git(repository, {"commit", "-q", "--allow-empty", "-m", "start"});
        const auto command = [&](const QString& type, const QJsonObject& payload) {
            return fixture.host->handleCommand(agent_test::request(type, payload), "desktop-test");
        };
        const QJsonObject created = command(
            "team.create",
            {{"repository", repository},
             {"name", "Shop"},
             {"integration_branch", "integration"},
             {"merge_policy", "user"},
             {"plan", QJsonObject{{"leader", QJsonObject{{"provider", "codex"}}},
                                  {"developers", QJsonObject{{"count", 1}, {"provider", "codex"}}},
                                  {"reviewer", QJsonObject{{"provider", "claude"}}}}}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(agent_test::text(created)));
        const QJsonArray members = created.value("team").toObject().value("members").toArray();
        const QString teamId = created.value("team").toObject().value("id").toString();
        const QString developer = members.at(1).toObject().value("session_id").toString();
        QVERIFY(command("team.task.create", {{"team_id", teamId}, {"title", "Cart no-findings"}})
                    .value("ok")
                    .toBool());
        const QJsonObject assigned = command(
            "team.task.assign", {{"team_id", teamId}, {"task_id", "T-1"}, {"owner", developer}});
        QVERIFY2(assigned.value("ok").toBool(), qPrintable(agent_test::text(assigned)));
        const QJsonObject task = assigned.value("task").toObject();
        const QString worktree = task.value("worktree").toString();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.count(developer, "turn.completed") >= 1, 8000);
        QVERIFY(fixture
                    .agents(developer, "agents.team_task_update",
                            {{"task_id", "T-1"}, {"state", "in_progress"}})
                    .value("ok")
                    .toBool());
        QFile file(QDir(worktree).filePath("cart.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("cart\n");
        file.close();
        git(worktree, {"add", "cart.txt"});
        git(worktree, {"commit", "-q", "-m", "Cart"});
        QVERIFY(fixture
                    .agents(developer, "agents.team_task_update",
                            {{"task_id", "T-1"}, {"state", "in_review"}})
                    .value("ok")
                    .toBool());
        ready.teamId = teamId;
        ready.repository = repository;
        ready.worktree = worktree;
        ready.branch = task.value("branch").toString();
        const auto read = [&] {
            return command("team.get", {{"team_id", teamId}}).value("team").toObject();
        };
        const auto state = [&] {
            return read().value("tasks").toArray().first().toObject().value("state").toString();
        };
        QTRY_COMPARE_WITH_TIMEOUT(state(), QStringLiteral("ready"), 10000);
        QCOMPARE(read().value("open_decisions").toInt(), 1);
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
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
    }

    void underTheLeaderAPassedTaskIsTheLeadersToMergeWithoutAQuestion() {
        const QString id = task("Cart");
        pass(id, "dev1");
        QVERIFY(merges().isEmpty());
        QCOMPARE(updates("merge_ready"), 1);
        QVERIFY(get(id).mergeOffered);
        // Not merged yet: a turn end changes nothing.
        teams_->memberTurnEnded("dev1");
        QCOMPARE(get(id).state, state::ready);
        host_.merged.insert(id);
        teams_->memberTurnEnded("lead");
        QCOMPARE(get(id).state, state::done);
        QCOMPARE(updates("goal_reached"), 1);
    }

    void underTheUserOneDecisionAllowsExactlyThatMerge() {
        QVERIFY(teams_->update(team_, {{"merge_policy", "user"}}, "user").ok());
        const QString id = task("Cart");
        pass(id, "dev1");
        QCOMPARE(merges().size(), 1);
        QCOMPARE(merges().first()->question, QStringLiteral("Merge T-1 \"Cart\"?"));
        QCOMPARE(merges().first()->choices.last(), leaderMergesChoice);
        QVERIFY(!get(id).leaderMayMerge);
        host_.messages.clear();
        QVERIFY(
            teams_->answerDecision(team_, merges().first()->id, leaderMergesChoice, "user").ok());
        QVERIFY(get(id).leaderMayMerge);
        QCOMPARE(host_.of("decision").size(), 1);
        QCOMPARE(host_.of("decision").first().toSession, QStringLiteral("lead"));
    }

    void aTaskWaitsForItsDependenciesUnderEitherSetting() {
        for (const QString& policy : {QStringLiteral("leader"), QStringLiteral("user")}) {
            init();
            QVERIFY(teams_->update(team_, {{"merge_policy", policy}}, "user").ok());
            const QString first = task("Api");
            const QString second = task("Cart");
            // The leader makes Cart depend on Api while Cart is already under way.
            QVERIFY(teams_->assignTask(team_, second, "dev2", "lead").ok());
            QVERIFY(teams_->moveTask(team_, second, state::inProgress, "dev2").ok());
            QVERIFY(teams_->updateTask(team_, second, {{"depends_on", QJsonArray{first}}}, "lead")
                        .ok());
            QVERIFY(teams_->moveTask(team_, second, state::inReview, "dev2").ok());
            finishReview(*teams_, "rev", "Verdict: no findings", true);
            QCOMPARE(get(second).state, state::ready);
            QVERIFY2(!get(second).mergeOffered, qPrintable(policy));
            QCOMPARE(merges().size(), 0);
            pass(first, "dev1");
            QVERIFY(get(first).mergeOffered);
            QVERIFY2(!get(second).mergeOffered, qPrintable(policy));
            host_.merged.insert(first);
            teams_->checkMerged(team_);
            QCOMPARE(get(first).state, state::done);
            QVERIFY2(get(second).mergeOffered, qPrintable(policy));
            QCOMPARE(merges().size(), policy == "user" ? 2 : 0);
        }
    }

    void aChangedSettingAppliesToTheNextReadyTaskAndIsLogged() {
        const QString first = task("Api");
        const QString second = task("Cart");
        pass(first, "dev1");
        QVERIFY(merges().isEmpty());
        QVERIFY(teams_->update(team_, {{"merge_policy", "user"}}, "user").ok());
        QCOMPARE(updates("merge_policy"), 1);
        QVERIFY(merges().isEmpty());
        pass(second, "dev2");
        QCOMPARE(merges().size(), 1);
        QCOMPARE(merges().first()->taskId, second);
    }

    void aMergeByHandMakesTheTaskDoneAndWithdrawsItsDecision() {
        QVERIFY(teams_->update(team_, {{"merge_policy", "user"}}, "user").ok());
        const QString id = task("Cart");
        pass(id, "dev1");
        QCOMPARE(merges().first()->state, QStringLiteral("open"));
        host_.merged.insert(id);
        teams_->checkMerged(team_);
        QCOMPARE(get(id).state, state::done);
        QCOMPARE(merges().first()->state, QStringLiteral("withdrawn"));
        QCOMPARE(team().openDecisions(), 0);
    }

    void commitsAfterTheReviewSendAReadyTaskBackToReview() {
        QVERIFY(teams_->update(team_, {{"merge_policy", "user"}}, "user").ok());
        const QString id = task("Cart");
        pass(id, "dev1");
        QCOMPARE(merges().size(), 1);
        const QString reviewed = get(id).headCommit;
        QVERIFY(!reviewed.isEmpty());
        // Nothing moved: it stays ready.
        teams_->memberTurnEnded("dev1");
        QCOMPARE(get(id).state, state::ready);
        host_.heads.insert(get(id).branch, "later");
        teams_->memberTurnEnded("dev1");
        QCOMPARE(get(id).state, state::blocked);
        QCOMPARE(get(id).resumeState, state::inProgress);
        QVERIFY(get(id).question.contains("later"));
        QVERIFY(!get(id).mergeOffered && !get(id).leaderMayMerge);
        QCOMPARE(merges().first()->state, QStringLiteral("withdrawn"));
        // Reviewed again at the new commit, it is offered again.
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inReview, "dev1").ok());
        finishReview(*teams_, "rev", "Verdict: no findings", true);
        QCOMPARE(get(id).state, state::ready);
        QCOMPARE(get(id).headCommit, QStringLiteral("later"));
        QCOMPARE(merges().size(), 2);
        teams_->memberTurnEnded("dev1");
        QCOMPARE(get(id).state, state::ready);
    }

    void aMergeWithCommitsAfterTheReviewIsDoneAndReported() {
        const QString id = task("Cart");
        pass(id, "dev1");
        host_.heads.insert(get(id).branch, "later");
        host_.merged.insert(id);
        teams_->memberTurnEnded("lead");
        QCOMPARE(get(id).state, state::done);
        QCOMPARE(updates("merged_unreviewed"), 1);
    }

    void theAncestorCheckOnARealRepository() {
        agent_test::Fixture fixture;
        Ready ready;
        readyOnARealRepository(fixture, ready);
        if (QTest::currentTestFailed())
            return;
        // Not contained yet; then the user merges by hand outside the app.
        QCOMPARE(stateOf(fixture, ready.teamId), QStringLiteral("ready"));
        // A worktree that leaves the branch does not change what was reviewed.
        git(ready.worktree, {"checkout", "-q", "--detach"});
        git(ready.worktree, {"commit", "-q", "--allow-empty", "-m", "Detached"});
        QCOMPARE(stateOf(fixture, ready.teamId), QStringLiteral("ready"));
        git(ready.repository, {"merge", "-q", "--no-ff", "-m", "Merge T-1", ready.branch});
        QCOMPARE(stateOf(fixture, ready.teamId), QStringLiteral("done"));
        QCOMPARE(readTeam(fixture, ready.teamId).value("open_decisions").toInt(), 0);
    }

    void aBranchMovedOutsideItsWorktreeIsSeenOnARealRepository() {
        agent_test::Fixture fixture;
        Ready ready;
        readyOnARealRepository(fixture, ready);
        if (QTest::currentTestFailed())
            return;
        // The worktree's HEAD stays where it was reviewed; the branch moves without it.
        git(ready.worktree, {"checkout", "-q", "--detach"});
        git(ready.repository, {"checkout", "-q", "-b", "elsewhere", ready.branch});
        git(ready.repository, {"commit", "-q", "--allow-empty", "-m", "Later"});
        git(ready.repository, {"branch", "-f", ready.branch, "elsewhere"});
        git(ready.repository, {"checkout", "-q", "integration"});
        QCOMPARE(stateOf(fixture, ready.teamId), QStringLiteral("blocked"));
        QCOMPARE(readTeam(fixture, ready.teamId).value("open_decisions").toInt(), 0);
    }
};

QTEST_GUILESS_MAIN(TeamMergeTest)
#include "TeamMergeTest.moc"
