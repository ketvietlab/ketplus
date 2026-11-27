// T-38: which work is a task's. A task gets a head only from a commit of its own branch after
// its base; git never marks done a task without one. T-10: the leader renames a lane; lanes
// no longer own paths. The coordinator cases use a fake runtime, the git cases a temporary
// repository. No network.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "runtime/TeamGit.h"
#include "storage/LocalJournal.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace ketplus::daemon;
using namespace team_test;

namespace {

const QString pullRequest = QStringLiteral("https://example.invalid/shop/pull/9");

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

void write(const QString& path, const QByteArray& text = "x\n") {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(text);
}

// A repository on `integration` with one commit, and a task worktree on its own branch.
struct Repository final {
    QTemporaryDir root;
    QString path;
    QString worktree;
    Team team;
    Task task;

    bool make() {
        path = QDir(root.path()).filePath("shop");
        worktree = QDir(root.path()).filePath("shop-t-1");
        QDir().mkpath(path);
        git(path, {"init", "-q", "-b", "integration"});
        write(QDir(path).filePath(".gitignore"), "build/\n");
        write(QDir(path).filePath("web/index.txt"));
        git(path, {"add", "-A"});
        git(path, {"commit", "-q", "-m", "start"});
        if (git(path, {"worktree", "add", "-q", "-b", "team/1/t-1", worktree}) ==
            QStringLiteral("<failed>"))
            return false;
        team.repository = path;
        team.integrationBranch = QStringLiteral("integration");
        task.id = QStringLiteral("T-1");
        task.branch = QStringLiteral("team/1/t-1");
        task.worktree = worktree;
        task.baseCommit = git(path, {"rev-parse", "HEAD"});
        return true;
    }
};

} // namespace

class TeamAttributionTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    QString task(const QString& title) {
        QString id;
        (void)teams_->createTask(team_, {.title = title, .lane = "Web"}, "lead", &id);
        return id;
    }
    QString branchFor(const QString& id) { return get(id).branch; }
    const Task& get(const QString& id) { return *teams_->team(team_)->task(id); }
    void start(const QString& id) {
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
    }
    QStringList historyKinds(const QString& id) {
        QStringList kinds;
        for (const HistoryEntry& entry : get(id).history)
            kinds.append(entry.kind);
        return kinds;
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
        teams_->setPullRequestPeriod(60 * 60 * 1000, 0);
        teams_->load();
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
    }

    // Criterion 3: a branch still at its base gives no head, and with no pull request the hand
    // to review says how to name the work.
    void aBranchAtItsBaseIsNotHandedToReview() {
        const QString id = task("Cart");
        start(id);
        host_.heads.insert(branchFor(id), get(id).baseCommit);
        const TeamResult refused = teams_->moveTask(team_, id, state::inReview, "dev1");
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error->code, QStringLiteral("no_commits"));
        QVERIFY(refused.error->message.contains(QStringLiteral("pull_request")));
        QCOMPARE(get(id).state, state::inProgress);
        QVERIFY(get(id).headCommit.isEmpty());
    }

    // Criterion 3: the commit another task was handed in at is not this task's work.
    void anotherTasksCommitGivesNoHead() {
        const QString first = task("Cart");
        start(first);
        host_.heads.insert(branchFor(first), QStringLiteral("c0ffee1"));
        QVERIFY(teams_->moveTask(team_, first, state::inReview, "dev1").ok());
        QCOMPARE(get(first).headCommit, QStringLiteral("c0ffee1"));
        const QString second = task("Checkout");
        QVERIFY(teams_->assignTask(team_, second, "dev2", "lead").ok());
        QVERIFY(teams_->moveTask(team_, second, state::inProgress, "dev2").ok());
        host_.heads.insert(branchFor(second), QStringLiteral("c0ffee1"));
        const TeamResult refused = teams_->moveTask(team_, second, state::inReview, "dev2");
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error->code, QStringLiteral("no_commits"));
    }

    // Criterion 5: work done in another worktree or repository is named by the hand to
    // review, and the review request reads it there.
    void aHandToReviewNamesWorkDoneElsewhere() {
        const QString id = task("Cart");
        start(id);
        host_.heads.insert(branchFor(id), get(id).baseCommit);
        QVERIFY(teams_->updateTask(team_, id, {{"pull_request", pullRequest}}, "dev1").ok());
        const TeamResult invalid =
            teams_->moveTask(team_, id, state::inReview, "dev1", {}, QStringLiteral("not a sha"));
        QCOMPARE(invalid.error->code, QStringLiteral("invalid_payload"));
        QVERIFY(
            teams_->moveTask(team_, id, state::inReview, "dev1", {}, QStringLiteral("abc1234def"))
                .ok());
        QCOMPARE(get(id).state, state::inReview);
        QCOMPARE(get(id).headCommit, QStringLiteral("abc1234def"));
        QVERIFY(historyKinds(id).contains(QStringLiteral("handoff")));
        QString body;
        for (const TeamMessage& message : host_.messages)
            if (message.kind == QStringLiteral("review") && message.taskId == id)
                body = message.body;
        QVERIFY2(body.contains(QStringLiteral("Pull request: ") + pullRequest), qPrintable(body));
        QVERIFY(body.contains(QStringLiteral("Head commit: abc1234def")));
    }

    // Criterion 4, the fault this team met: a task whose worktree was never committed to has
    // its branch at its base, which the integration branch contains. With its pull request
    // open, being reviewed and ready does not make it done.
    void aNeverCommittedTaskIsNotDoneBecauseIntegrationContainsItsBase() {
        const QString id = task("Cart");
        start(id);
        host_.heads.insert(branchFor(id), get(id).baseCommit);
        QVERIFY(teams_->updateTask(team_, id, {{"pull_request", pullRequest}}, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inReview, "dev1").ok());
        finishReview(*teams_, "rev", "Verdict: no findings", true);
        QCOMPARE(get(id).state, state::ready);
        host_.merged.insert(id);
        teams_->checkMerged(team_);
        QCOMPARE(get(id).state, state::ready);
        QVERIFY(!historyKinds(id).contains(QStringLiteral("merged")));
        // Still not after the pull request is left out: git speaks only of the branch's own
        // commits.
        QVERIFY(teams_->updateTask(team_, id, {{"pull_request", QString{}}}, "lead").ok());
        teams_->checkMerged(team_);
        QCOMPARE(get(id).state, state::ready);
    }

    // Criterion 3 on git: a worktree at its base, at the integration branch or at a commit the
    // integration branch has gives no head and no diff; a commit of its own branch does.
    void ownHeadOnARealRepository() {
        Repository repository;
        QVERIFY(repository.make());
        const Team& team = repository.team;
        Task& task = repository.task;
        QCOMPARE(team_git::ownHead(team, task), std::optional<QString>(QString{}));
        QVERIFY(team_git::isMerged(team, task).value_or(false));
        // The integration branch moves on and the worktree is checked out there.
        write(QDir(repository.path).filePath("api/routes.txt"));
        git(repository.path, {"add", "-A"});
        git(repository.path, {"commit", "-q", "-m", "someone else"});
        git(repository.worktree, {"checkout", "-q", "--detach", "integration"});
        QCOMPARE(team_git::ownHead(team, task), std::optional<QString>(QString{}));
        const auto none = team_git::taskDiff(team, task);
        QVERIFY(none && none->outsideBranch && none->files.isEmpty());
        // A commit of its own branch is its head, and its diff is the task's alone.
        git(repository.worktree, {"checkout", "-q", "team/1/t-1"});
        write(QDir(repository.worktree).filePath("web/cart.txt"));
        git(repository.worktree, {"add", "-A"});
        git(repository.worktree, {"commit", "-q", "-m", "Cart"});
        const QString head = git(repository.worktree, {"rev-parse", "HEAD"});
        QCOMPARE(team_git::ownHead(team, task), std::optional<QString>(head));
        task.headCommit = head;
        const auto diff = team_git::taskDiff(team, task);
        QVERIFY(diff && !diff->outsideBranch);
        QCOMPARE(diff->files, QStringList{"web/cart.txt"});
        // Once the integration branch has it, nothing on it is left to hand in.
        git(repository.path, {"merge", "-q", "--no-ff", "-m", "Merge", "team/1/t-1"});
        QCOMPARE(team_git::ownHead(team, task), std::optional<QString>(QString{}));
        // Moved to a commit that does not come after its base.
        task.baseCommit = head;
        git(repository.worktree, {"checkout", "-q", "--detach"});
        git(repository.path, {"branch", "-f", "team/1/t-1", "integration~1"});
        QCOMPARE(team_git::ownHead(team, task), std::optional<QString>(QString{}));
    }

    // A branch git cannot read is said as such, not as a branch without commits.
    void anUnreadableBranchIsNotABranchWithoutCommits() {
        Repository repository;
        QVERIFY(repository.make());
        // Git cannot answer: the repository is gone.
        Team gone = repository.team;
        gone.repository = QDir(repository.root.path()).filePath("not-a-repository");
        QVERIFY(QDir().mkpath(gone.repository));
        QVERIFY(!team_git::ownHead(gone, repository.task));
        QVERIFY(!team_git::handoff(gone, repository.task).headRead);
        // Git answers that the branch is not there: no head of its own, and read.
        Task task = repository.task;
        task.branch = QStringLiteral("team/1/missing");
        QCOMPARE(team_git::ownHead(repository.team, task), std::optional<QString>(QString{}));
        QVERIFY(team_git::handoff(repository.team, task).headRead);
        const auto diff = team_git::taskDiff(repository.team, task);
        QVERIFY(diff && diff->outsideBranch);
        // The coordinator refuses the hand to review with what happened.
        const QString id = this->task("Cart");
        start(id);
        host_.unreadableHeads.insert(get(id).branch);
        const TeamResult refused = teams_->moveTask(team_, id, state::inReview, "dev1");
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error->code, QStringLiteral("git_unavailable"));
        QCOMPARE(get(id).state, state::inProgress);
    }

    // Criterion 5 with the branch deleted: the hand to review names the pull request and
    // its head commit and is accepted; without the pull request it says what is missing.
    void aDeletedBranchIsHandedInWithItsPullRequest() {
        Repository repository;
        QVERIFY(repository.make());
        git(repository.worktree, {"checkout", "-q", "--detach"});
        git(repository.path, {"branch", "-D", "team/1/t-1"});
        const auto facts = team_git::handoff(repository.team, repository.task);
        QVERIFY(facts.headRead);
        QVERIFY(facts.head.isEmpty());
        // The coordinator with that answer.
        const QString id = task("Cart");
        start(id);
        host_.heads.insert(get(id).branch, get(id).baseCommit);
        QCOMPARE(teams_->moveTask(team_, id, state::inReview, "dev1").error->code,
                 QStringLiteral("no_commits"));
        QVERIFY(teams_->updateTask(team_, id, {{"pull_request", pullRequest}}, "dev1").ok());
        QVERIFY(
            teams_->moveTask(team_, id, state::inReview, "dev1", {}, QStringLiteral("0123abc4567"))
                .ok());
        QCOMPARE(get(id).state, state::inReview);
        QCOMPARE(get(id).headCommit, QStringLiteral("0123abc4567"));
    }

    // T-10: the leader renames a lane with a tool, refused to the other roles.
    void theLeaderRenamesALaneWithATool() {
        agent_test::Fixture fixture;
        QVERIFY(fixture.open());
        const QString repository = fixture.directory.filePath("shop");
        QDir().mkpath(repository);
        git(repository, {"init", "-q", "-b", "integration"});
        git(repository, {"commit", "-q", "--allow-empty", "-m", "start"});
        const QJsonObject created = fixture.host->handleCommand(
            agent_test::request(
                "team.create",
                {{"repository", repository},
                 {"name", "Shop"},
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
        QVERIFY(fixture.agents(leader, "agents.team_task_create", {{"title", "Cart"}})
                    .value("ok")
                    .toBool());
        const QString number = fixture.number(developer);
        QCOMPARE(fixture
                     .agents(developer, "agents.team_lane_set",
                             {{"developer", number}, {"lane", "Shop"}, {"reason", "wider"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("role_forbidden"));
        QCOMPARE(fixture
                     .agents(leader, "agents.team_lane_set",
                             {{"developer", number}, {"lane", "Storefront"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("invalid_payload"));
        const QJsonObject renamed = fixture.agents(leader, "agents.team_lane_set",
                                                   {{"developer", number},
                                                    {"lane", "Storefront"},
                                                    {"paths", QJsonArray{"web"}},
                                                    {"reason", "the cart and more"}});
        QVERIFY2(renamed.value("ok").toBool(), qPrintable(agent_test::text(renamed)));
        const QJsonObject team = fixture.agents(leader, "agents.team_get").value("team").toObject();
        QCOMPARE(team.value("members").toArray().at(1).toObject().value("lane").toString(),
                 QStringLiteral("Storefront"));
        QVERIFY(!team.contains("shared_paths"));
        QVERIFY(!team.value("members").toArray().at(1).toObject().contains("paths"));
        // T-10: the tool that allowed paths is gone.
        QVERIFY(!fixture
                     .agents(leader, "agents.team_paths_allow",
                             {{"paths", QJsonArray{"contracts"}}, {"reason", "generated"}})
                     .value("ok")
                     .toBool());
    }
};

QTEST_GUILESS_MAIN(TeamAttributionTest)
#include "TeamAttributionTest.moc"
