// ADR 0114 §4: lanes, the check outside a lane and lane changes. The coordinator cases use a
// fake runtime; the end-to-end case a temporary Git repository and the scripted fake
// providers. No real provider is started.
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
    process.start("git", arguments);
    QVERIFY(process.waitForFinished(15000));
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
}

void write(const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x\n");
}

} // namespace

class TeamLaneTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};
    QString task_;

    const Team& team() { return *teams_->team(team_); }
    const Task& task() { return *team().task(task_); }
    QList<TeamUpdateRecord> updates(const QString& kind) {
        QList<TeamUpdateRecord> result;
        for (const TeamUpdateRecord& update : journal_->teamUpdates(team_, false, 200))
            if (update.kind == kind)
                result.append(update);
        return result;
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
        Team draft = standardTeam();
        draft.sharedPaths = {"tests"};
        team_ = teams_->create(draft, "user").teamId;
        QVERIFY(team_ > 0);
        QVERIFY(teams_->createTask(team_, {.title = "Cart", .lane = "Web"}, "lead", &task_).ok());
        QVERIFY(teams_->assignTask(team_, task_, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, task_, state::inProgress, "dev1").ok());
    }

    void insideTheLaneOrUnderASharedPathRaisesNoFlag() {
        host_.changed.insert(task_, {"web/cart.cpp", "web/deep/a.h", "tests/cart_test.cpp"});
        teams_->memberTurnEnded("dev1");
        QVERIFY(task().outsideLane.isEmpty());
        QVERIFY(updates("outside_lane").isEmpty());
    }

    void outsideTheLaneIsFlaggedAndReachesTheDigest() {
        host_.changed.insert(task_, {"web/cart.cpp", "api/routes.cpp", "webby/x"});
        teams_->memberTurnEnded("dev1");
        QCOMPARE(task().outsideLane, (QStringList{"api/routes.cpp", "webby/x"}));
        QCOMPARE(updates("outside_lane").size(), 1);
        QVERIFY(updates("outside_lane").first().actionable);
        // The same paths again are no news.
        teams_->memberTurnEnded("dev1");
        QCOMPARE(updates("outside_lane").size(), 1);
        // It blocks nothing, and the leader's next digest lists the paths.
        QCOMPARE(task().state, state::inProgress);
        host_.messages.clear();
        teams_->memberTurnEnded("lead");
        QCOMPARE(host_.of("digest").size(), 1);
        QVERIFY(host_.of("digest").first().body.contains("api/routes.cpp"));
    }

    void revertingTheFilesClearsTheFlag() {
        host_.changed.insert(task_, {"api/routes.cpp"});
        teams_->memberTurnEnded("dev1");
        QCOMPARE(task().outsideLane.size(), 1);
        host_.changed.insert(task_, {"web/cart.cpp"});
        teams_->memberTurnEnded("dev1");
        QVERIFY(task().outsideLane.isEmpty());
        QCOMPARE(updates("outside_lane_cleared").size(), 1);
        QVERIFY(!updates("outside_lane_cleared").first().actionable);
        QVERIFY(task().history.last().text.contains("reverted"));
    }

    void aLaneChangeIsLoggedAndCoversFlaggedFiles() {
        host_.changed.insert(task_, {"docs/cart.md"});
        teams_->memberTurnEnded("dev1");
        QCOMPARE(task().outsideLane, QStringList{"docs/cart.md"});
        QVERIFY(teams_
                    ->updateMember(team_, "dev1", "Web", {"web", "docs"}, "lead",
                                   "the cart's docs live with it")
                    .ok());
        QVERIFY(task().outsideLane.isEmpty());
        const auto changed = updates("lane_changed");
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.first().actor, QStringLiteral("lead"));
        QVERIFY(changed.first().text.contains("the cart's docs live with it"));
        QCOMPARE(changed.first().data.value("from").toArray().size(), 1);
        bool logged = false;
        for (const HistoryEntry& entry : task().history)
            logged = logged || (entry.kind == "lane" && entry.actor == "lead" &&
                                entry.text.contains("the cart's docs live with it"));
        QVERIFY(logged);
        // Shared paths cover flagged files the same way.
        host_.changed.insert(task_, {"build/cart.cmake"});
        teams_->memberTurnEnded("dev1");
        QCOMPARE(task().outsideLane.size(), 1);
        QVERIFY(
            teams_->update(team_, {{"shared_paths", QJsonArray{"tests", "build"}}}, "user").ok());
        QVERIFY(task().outsideLane.isEmpty());
    }

    void aPrefixBelongsToOneLane() {
        const auto refused =
            teams_->updateMember(team_, "dev1", "Web", {"web", "api/v2"}, "lead", "take the api");
        QCOMPARE(refused.error.value_or(TeamError{}).code, QStringLiteral("lane_overlap"));
        QCOMPARE(team().member("dev1")->paths, QStringList{"web"});
    }

    void theCheckRunsOnTheTaskWorktreeAfterADevelopersTurn() {
        agent_test::Fixture fixture;
        QVERIFY(fixture.open(true));
        const QString repository = fixture.directory.filePath("shop");
        QVERIFY(QDir().mkpath(repository));
        git(repository, {"init", "-q", "-b", "integration"});
        git(repository, {"-c", "user.email=t@example.com", "-c", "user.name=T", "commit", "-q",
                         "--allow-empty", "-m", "start"});
        const auto command = [&](const QString& type, const QJsonObject& payload) {
            return fixture.host->handleCommand(agent_test::request(type, payload), "desktop-test");
        };
        const QJsonObject created = command(
            "team.create",
            {{"repository", repository},
             {"name", "Shop"},
             {"integration_branch", "integration"},
             {"shared_paths", QJsonArray{"tests"}},
             {"plan",
              QJsonObject{
                  {"leader", QJsonObject{{"provider", "codex"}}},
                  {"developers",
                   QJsonObject{{"count", 1},
                               {"provider", "codex"},
                               {"members", QJsonArray{QJsonObject{{"paths", QJsonArray{"web"}}}}}}},
                  {"reviewer", QJsonObject{{"provider", "claude"}}}}}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(agent_test::text(created)));
        const QJsonArray members = created.value("team").toObject().value("members").toArray();
        const QString teamId = created.value("team").toObject().value("id").toString();
        const QString leader = members.at(0).toObject().value("session_id").toString();
        const QString developer = members.at(1).toObject().value("session_id").toString();
        QVERIFY(command("team.task.create", {{"team_id", teamId}, {"title", "Cart"}})
                    .value("ok")
                    .toBool());
        const QJsonObject assigned = command(
            "team.task.assign", {{"team_id", teamId}, {"task_id", "T-1"}, {"owner", developer}});
        QVERIFY2(assigned.value("ok").toBool(), qPrintable(agent_test::text(assigned)));
        const QString worktree = assigned.value("task").toObject().value("worktree").toString();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.count(developer, "turn.completed") >= 1, 8000);
        QVERIFY(fixture
                    .agents(developer, "agents.team_task_update",
                            {{"task_id", "T-1"}, {"state", "in_progress"}})
                    .value("ok")
                    .toBool());
        const auto outside = [&] {
            return command("team.task.get", {{"team_id", teamId}, {"task_id", "T-1"}})
                .value("task")
                .toObject()
                .value("outside_lane")
                .toArray();
        };
        // One file outside (uncommitted), one inside, one shared; then a developer turn.
        write(QDir(worktree).filePath("api/routes.txt"));
        write(QDir(worktree).filePath("web/cart.txt"));
        write(QDir(worktree).filePath("tests/cart.txt"));
        const auto ping = [&](const int turns) {
            QVERIFY(fixture
                        .agents(leader, "agents.send",
                                {{"to", fixture.number(developer)}, {"text", "Status?"}})
                        .value("ok")
                        .toBool());
            QTRY_VERIFY_WITH_TIMEOUT(fixture.count(developer, "turn.completed") >= turns, 8000);
        };
        ping(2);
        QTRY_COMPARE_WITH_TIMEOUT(outside().size(), 1, 5000);
        QCOMPARE(outside().first().toString(), QStringLiteral("api/routes.txt"));
        // Reverting the file clears the flag after the next turn, without anyone's action.
        QVERIFY(QFile::remove(QDir(worktree).filePath("api/routes.txt")));
        ping(3);
        QTRY_VERIFY_WITH_TIMEOUT(outside().isEmpty(), 5000);
    }
};

QTEST_GUILESS_MAIN(TeamLaneTest)
#include "TeamLaneTest.moc"
