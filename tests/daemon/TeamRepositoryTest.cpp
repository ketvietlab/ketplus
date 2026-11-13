// T-90 (ADR 0121): a team moves to another repository, and a task can work in a repository of
// its own. The coordinator cases use a fake runtime; the git cases temporary repositories with
// a bare remote; the leader's tool the session runtime with the scripted fake providers. No
// network, no real provider and no real team.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "runtime/TeamGit.h"
#include "storage/LocalJournal.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace ketplus::daemon;
using namespace team_test;

namespace {

const QString evidence = QStringLiteral("0123456789abcdef0123456789abcdef01234567");

QString git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git",
                  QStringList{"-c", "user.email=t@example.com", "-c", "user.name=T"} + arguments);
    if (!process.waitForFinished(15000) || process.exitCode() != 0) {
        qWarning("%s", process.readAllStandardError().constData());
        return QStringLiteral("<failed>");
    }
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

// A repository on `branch` with one commit and, unless `withOrigin` is false, a bare origin
// that has the branch.
QString makeRepository(const QString& root, const QString& name,
                       const QString& branch = QStringLiteral("integration"),
                       const bool withOrigin = true) {
    const QString path = QDir(root).filePath(name);
    QDir().mkpath(path);
    git(path, {"init", "-q", "-b", branch});
    git(path, {"commit", "-q", "--allow-empty", "-m", "start"});
    if (withOrigin) {
        const QString remote = QDir(root).filePath(name + ".git");
        git(root, {"init", "-q", "--bare", remote});
        git(path, {"remote", "add", "origin", remote});
        git(path, {"push", "-q", "origin", branch});
    }
    return QFileInfo(path).canonicalFilePath();
}

} // namespace

class TeamRepositoryTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    const Team& team() { return *teams_->team(team_); }
    const Task& get(const QString& id) { return *team().task(id); }
    QString create(const TaskDraft& draft, TeamResult* result = nullptr) {
        QString id;
        const TeamResult made = teams_->createTask(team_, draft, "lead", &id);
        if (result)
            *result = made;
        return id;
    }
    QList<TeamUpdateRecord> updates(const QString& kind) {
        QList<TeamUpdateRecord> result;
        for (const TeamUpdateRecord& update : journal_->teamUpdates(team_, false, 200))
            if (update.kind == kind)
                result.append(update);
        return result;
    }
    TeamResult move(const QString& repository, const QString& branch = {},
                    const QString& actor = QStringLiteral("user")) {
        QJsonObject changes{{"repository", repository}, {"reason", "The old one is archived."}};
        if (!branch.isEmpty())
            changes.insert("integration_branch", branch);
        return teams_->update(team_, changes, actor);
    }
    void reload() {
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(60 * 60 * 1000);
        teams_->setPullRequestPeriod(60 * 60 * 1000, 0);
        teams_->load();
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        teams_.reset();
        journal_.reset();
        QFile::remove(directory_.filePath("journal.sqlite3"));
        QDir(directory_.filePath("wt")).removeRecursively();
        host_ = {};
        host_.worktreeRoot = directory_.filePath("wt") + "/";
        for (const QString& id : {"lead", "dev1", "dev2"})
            host_.add(id);
        host_.add("rev", "claude");
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        reload();
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
    }

    void aTaskSeesItsOwnRepositoryThroughItsScope() {
        const Team whole = standardTeam();
        Task task;
        QCOMPARE(taskScope(whole, task).repository, QStringLiteral("/repo"));
        QCOMPARE(taskScope(whole, task).integrationBranch, QStringLiteral("integration"));
        task.repository = QStringLiteral("/mobile");
        task.integrationBranch = QStringLiteral("develop");
        QCOMPARE(taskScope(whole, task).repository, QStringLiteral("/mobile"));
        QCOMPARE(taskScope(whole, task).integrationBranch, QStringLiteral("develop"));
        // A task's own repository goes to clients; none means the team's.
        QCOMPARE(toJson(task, false).value("repository").toString(), QStringLiteral("/mobile"));
        QVERIFY(!toJson(Task{}, false).contains("repository"));
    }

    // Criteria 2 and 3: the change is recorded with its reason; the started task keeps the
    // repository it began in, the task not started yet and a new one use the new repository.
    void startedTasksKeepTheirRepositoryAndNewOnesUseTheNewOne() {
        const QString started = create({.title = "Cart", .lane = "Web"});
        const QString waiting = create({.title = "Search", .lane = "Web"});
        QVERIFY(teams_->assignTask(team_, started, "dev1", "lead").ok());
        QCOMPARE(host_.madeIn, QStringList{"/repo"});
        const QString worktree = get(started).worktree;
        const QString branch = get(started).branch;

        const TeamResult moved = move("/ketplus", "main");
        QVERIFY2(moved.ok(), moved.ok() ? "" : qPrintable(moved.error->message));
        QCOMPARE(team().repository, QStringLiteral("/ketplus"));
        QCOMPARE(team().integrationBranch, QStringLiteral("main"));
        QCOMPARE(host_.repositoryChecks.last(),
                 std::make_pair(QStringLiteral("/ketplus"), QStringLiteral("main")));
        // Kept where it began: nothing on disk moved, its branch and worktree are the same.
        QCOMPARE(get(started).repository, QStringLiteral("/repo"));
        QCOMPARE(get(started).integrationBranch, QStringLiteral("integration"));
        QCOMPARE(get(started).worktree, worktree);
        QCOMPARE(get(started).branch, branch);
        QCOMPARE(get(started).history.last().kind, QStringLiteral("repository"));
        QVERIFY(get(waiting).repository.isEmpty());

        const auto recorded = updates("repository");
        QCOMPARE(recorded.size(), 1);
        QVERIFY(recorded.first().text.contains("/ketplus"));
        QVERIFY(recorded.first().text.contains("The old one is archived."));
        QCOMPARE(recorded.first().data.value("from").toString(), QStringLiteral("/repo"));
        QCOMPARE(recorded.first().data.value("to").toString(), QStringLiteral("/ketplus"));
        QCOMPARE(recorded.first().data.value("kept").toArray(), QJsonArray{started});

        QVERIFY(teams_->assignTask(team_, waiting, "dev2", "lead").ok());
        const QString fresh = create({.title = "Checkout", .lane = "Web"});
        QVERIFY(teams_->assignTask(team_, fresh, "dev1", "lead").ok());
        QCOMPARE(host_.madeIn, (QStringList{"/repo", "/ketplus", "/ketplus"}));
        QVERIFY(get(waiting).repository.isEmpty());
        QVERIFY(get(fresh).repository.isEmpty());

        // Stored: a coordinator loaded again reads the same, and lists the team by its new
        // repository.
        reload();
        QCOMPARE(team().repository, QStringLiteral("/ketplus"));
        QCOMPARE(get(started).repository, QStringLiteral("/repo"));
        QCOMPARE(teams_->teams("/ketplus").size(), 1);
        QVERIFY(teams_->teams("/repo").isEmpty());

        // The same repository and branch again is no change and records nothing.
        QVERIFY(move("/ketplus", "main").ok());
        QCOMPARE(updates("repository").size(), 1);
    }

    // Criterion 2: a refused repository changes nothing and says why.
    void anInvalidRepositoryIsRefusedAndChangesNothing() {
        const QString started = create({.title = "Cart", .lane = "Web"});
        QVERIFY(teams_->assignTask(team_, started, "dev1", "lead").ok());
        const qint64 revision = team().revision;
        host_.repositoryErrors.insert("/notes", "/notes is not a Git repository.");
        TeamResult refused = move("/notes");
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error->code, QStringLiteral("invalid_repository"));
        QCOMPARE(refused.error->message, QStringLiteral("/notes is not a Git repository."));
        refused = move("  ");
        QCOMPARE(refused.error->code, QStringLiteral("invalid_repository"));
        refused =
            teams_->update(team_, {{"repository", "/ketplus"}, {"integration_branch", ""}}, "user");
        QCOMPARE(refused.error->code, QStringLiteral("invalid_payload"));
        refused = move("/ketplus", "bad..branch");
        QVERIFY(!refused.ok());
        QCOMPARE(team().repository, QStringLiteral("/repo"));
        QCOMPARE(team().integrationBranch, QStringLiteral("integration"));
        QCOMPARE(team().revision, revision);
        QVERIFY(get(started).repository.isEmpty());
        QVERIFY(updates("repository").isEmpty());
    }

    // Criterion 3: a task names its own repository; its worktree is made there and the team's
    // stays the default.
    void aTaskCanWorkInARepositoryOfItsOwn() {
        const QString own = create(
            {.title = "Phone", .lane = "Web", .repository = "/mobile", .integrationBranch = "dev"});
        QCOMPARE(get(own).repository, QStringLiteral("/mobile"));
        QCOMPARE(get(own).integrationBranch, QStringLiteral("dev"));
        QVERIFY(get(own).history.first().text.contains("/mobile"));
        // Its integration branch defaults to the team's.
        const QString shared = create({.title = "Widget", .lane = "Web", .repository = "/kit"});
        QCOMPARE(get(shared).integrationBranch, QStringLiteral("integration"));
        // Naming the team's own repository and branch is the default, stored as none.
        const QString same = create({.title = "Cart", .lane = "Web", .repository = "/repo"});
        QVERIFY(get(same).repository.isEmpty());

        QVERIFY(teams_->assignTask(team_, own, "dev1", "lead").ok());
        QCOMPARE(host_.madeIn, QStringList{"/mobile"});
        QCOMPARE(team().repository, QStringLiteral("/repo"));

        // Moving the team leaves a task's own repository alone.
        QVERIFY(move("/ketplus").ok());
        QCOMPARE(get(own).repository, QStringLiteral("/mobile"));
        QVERIFY(updates("repository").first().data.value("kept").toArray().isEmpty());

        // Refused: a branch without a repository, and a repository that is not one. A refused
        // task takes no number.
        TeamResult refused;
        create({.title = "Lost", .lane = "Web", .integrationBranch = "dev"}, &refused);
        QCOMPARE(refused.error->code, QStringLiteral("invalid_payload"));
        host_.repositoryErrors.insert("/notes", "/notes is not a Git repository.");
        create({.title = "Lost", .lane = "Web", .repository = "/notes"}, &refused);
        QCOMPARE(refused.error->code, QStringLiteral("invalid_repository"));
        QCOMPARE(create({.title = "Next", .lane = "Web"}), QStringLiteral("T-4"));
    }

    // Criterion 3: a worktree made while the team moved is kept with the repository it was
    // made in, so the task never reads the wrong one.
    void aWorktreeMadeInTheOldRepositoryIsKeptWithIt() {
        const QString id = create({.title = "Cart", .lane = "Web"});
        Team stale = team();
        Task task = get(id);
        takeWorktree(stale, task,
                     {.branch = "team/1/t-1",
                      .worktree = "/wt/T-1",
                      .baseCommit = "base",
                      .repository = "/repo",
                      .integrationBranch = "integration"});
        QVERIFY(task.repository.isEmpty());
        stale.repository = QStringLiteral("/ketplus");
        Task moved = get(id);
        takeWorktree(stale, moved,
                     {.branch = "team/1/t-1",
                      .worktree = "/wt/T-1",
                      .baseCommit = "base",
                      .repository = "/repo",
                      .integrationBranch = "integration"});
        QCOMPARE(moved.repository, QStringLiteral("/repo"));
        QCOMPARE(moved.integrationBranch, QStringLiteral("integration"));
    }

    // Criterion 3: the cleanup of a done task's worktree runs in the repository it worked in.
    void cleanupFollowsTheTasksRepository() {
        const QString id = create({.title = "Cart", .lane = "Web"});
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(QDir().mkpath(get(id).worktree));
        QVERIFY(teams_->closeTask(team_, id, evidence, "Merged.", "lead").ok());
        QVERIFY(move("/ketplus").ok());
        TeamResult result{.error = TeamError{"unanswered", {}}, .teamId = team_};
        teams_->removeWorktree(team_, id, "user",
                               [&](const TeamResult& r, const QString&) { result = r; });
        QVERIFY(result.ok());
        QCOMPARE(host_.removals.size(), 1);
        QCOMPARE(host_.removals.first().first, QStringLiteral("/repo"));
    }

    // Criterion 2: git refuses what is not a repository's main checkout with origin's branch.
    void gitChecksTheRepository() {
        QTemporaryDir root;
        const QString shop = makeRepository(root.path(), "shop");
        QCOMPARE(team_git::checkRepository(shop, "integration").repository, shop);
        QVERIFY(team_git::checkRepository(shop, "integration").error.isEmpty());
        // Without an integration branch any repository will do.
        QCOMPARE(team_git::checkRepository(shop, {}).repository, shop);

        QVERIFY(team_git::checkRepository("shop", "integration").error.contains("full path"));
        QVERIFY(team_git::checkRepository(root.filePath("none"), "integration")
                    .error.contains("not a folder"));
        QDir().mkpath(root.filePath("notes"));
        QVERIFY(team_git::checkRepository(root.filePath("notes"), "integration")
                    .error.contains("not a Git repository"));
        QDir().mkpath(QDir(shop).filePath("web"));
        QVERIFY(team_git::checkRepository(QDir(shop).filePath("web"), "integration")
                    .error.contains("main checkout"));
        const QString linked = root.filePath("shop-t-1");
        git(shop, {"worktree", "add", "-q", "-b", "t-1", linked});
        QVERIFY(team_git::checkRepository(linked, "integration").error.contains("main checkout"));
        QVERIFY(team_git::checkRepository(shop, "develop").error.contains("origin has no branch"));
        const QString lonely = makeRepository(root.path(), "lonely", "integration", false);
        QVERIFY(team_git::checkRepository(lonely, "integration").error.contains("no remote"));
    }

    // Criterion 3: git makes the worktree beside the task's repository, from its integration
    // branch, and reads the merge there.
    void gitWorksInTheTasksRepository() {
        QTemporaryDir root;
        const QString shop = makeRepository(root.path(), "shop");
        const QString mobile = makeRepository(root.path(), "mobile", "develop");
        Team whole;
        whole.id = 7;
        whole.repository = shop;
        whole.integrationBranch = QStringLiteral("integration");
        Task task;
        task.id = QStringLiteral("T-2");
        task.repository = mobile;
        task.integrationBranch = QStringLiteral("develop");
        const auto made = team_git::createWorktree(whole, task);
        QVERIFY2(made.worktree, qPrintable(made.error));
        QCOMPARE(made.worktree->repository, mobile);
        QCOMPARE(made.worktree->integrationBranch, QStringLiteral("develop"));
        QVERIFY(made.worktree->worktree.startsWith(mobile + "-team-7-"));
        QCOMPARE(made.worktree->baseCommit, git(mobile, {"rev-parse", "develop"}));
        task.branch = made.worktree->branch;
        task.worktree = made.worktree->worktree;
        task.baseCommit = made.worktree->baseCommit;
        QCOMPARE(team_git::branchExists(whole, task), std::optional<bool>(true));
        QCOMPARE(git(shop, {"branch", "--list", task.branch}), QString());
        // Merged into the task's integration branch, which the team's repository never had.
        QCOMPARE(team_git::isMerged(whole, task), std::optional<bool>(true));
        git(task.worktree, {"commit", "-q", "--allow-empty", "-m", "work"});
        QCOMPARE(team_git::isMerged(whole, task), std::optional<bool>(false));
        git(mobile, {"merge", "-q", "--ff-only", task.branch});
        QCOMPARE(team_git::isMerged(whole, task), std::optional<bool>(true));
    }

    // Criterion 2: the leader's tool, with its reason, refused to the other roles; and the
    // optional repository of team_task_create.
    void theLeaderMovesTheTeamWithATool() {
        agent_test::Fixture fixture;
        QVERIFY(fixture.open());
        const QString old = makeRepository(fixture.directory.path(), "legacy");
        const QString next = makeRepository(fixture.directory.path(), "ketplus");
        const QJsonObject created = fixture.host->handleCommand(
            agent_test::request(
                "team.create",
                {{"repository", old},
                 {"name", "QA"},
                 {"integration_branch", "integration"},
                 {"plan",
                  QJsonObject{{"leader", QJsonObject{{"provider", "claude"}}},
                              {"developers", QJsonObject{{"count", 1}, {"provider", "codex"}}},
                              {"reviewer", QJsonObject{{"provider", "claude"}}}}}}),
            "desktop-test");
        const QJsonArray members = created.value("team").toObject().value("members").toArray();
        QCOMPARE(members.size(), 3);
        const QString leader = members.at(0).toObject().value("session_id").toString();
        const QString developer = members.at(1).toObject().value("session_id").toString();

        QCOMPARE(fixture
                     .agents(developer, "agents.team_repository_set",
                             {{"repository", next}, {"reason", "moved"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("role_forbidden"));
        QCOMPARE(fixture.agents(leader, "agents.team_repository_set", {{"repository", next}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("invalid_payload"));
        const QJsonObject wrong =
            fixture.agents(leader, "agents.team_repository_set",
                           {{"repository", next}, {"integration_branch", "nope"}, {"reason", "x"}});
        QCOMPARE(wrong.value("error_code").toString(), QStringLiteral("invalid_repository"));
        QVERIFY(wrong.value("error_message").toString().contains("origin has no branch nope"));

        const QJsonObject moved = fixture.agents(
            leader, "agents.team_repository_set",
            {{"repository", next}, {"reason", "The legacy repository is archived."}});
        QVERIFY2(moved.value("ok").toBool(), qPrintable(agent_test::text(moved)));
        const QJsonObject team = fixture.agents(leader, "agents.team_get").value("team").toObject();
        QCOMPARE(team.value("repository").toString(), next);
        QCOMPARE(team.value("integration_branch").toString(), QStringLiteral("integration"));

        const QJsonObject own = fixture.agents(leader, "agents.team_task_create",
                                               {{"title", "Legacy fix"}, {"repository", old}});
        QVERIFY2(own.value("ok").toBool(), qPrintable(agent_test::text(own)));
        QCOMPARE(own.value("task").toObject().value("repository").toString(), old);
    }
};

QTEST_GUILESS_MAIN(TeamRepositoryTest)
#include "TeamRepositoryTest.moc"
