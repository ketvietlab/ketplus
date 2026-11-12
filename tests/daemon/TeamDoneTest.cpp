// ADR 0118: a task is done when its pull request is merged on the remote, or when the leader
// closes it by hand. The coordinator cases use a fake runtime and a fake remote; the probe and
// the git cases a temporary repository and a stand-in for the GitHub tool. No network.
#include "TeamFakeHost.h"
#include "runtime/TeamGit.h"
#include "runtime/TeamPullRequests.h"
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

const QString pullRequest = QStringLiteral("https://example.invalid/shop/pull/7");
const QString mergeCommit = QStringLiteral("0123456789abcdef0123456789abcdef01234567");

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

} // namespace

class TeamDoneTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    void open() {
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(60 * 60 * 1000);
        teams_->setPullRequestPeriod(60 * 60 * 1000, 0);
        teams_->load();
    }
    QString task(const QString& title, const QStringList& depends = {}) {
        QString id;
        (void)teams_->createTask(team_, {.title = title, .lane = "Web", .dependsOn = depends},
                                 "lead", &id);
        return id;
    }
    void recordPullRequest(const QString& id, const QString& url = pullRequest) {
        QVERIFY(teams_->updateTask(team_, id, {{"pull_request", url}}, "lead").ok());
    }
    // Brings a new task to `to`, with dev1 as its owner from In progress on.
    void bringTo(const QString& id, const QString& to) {
        if (to == state::todo)
            return;
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        if (to == state::blocked)
            QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", "Which colour?").ok());
        if (to == state::inReview || to == state::ready)
            QVERIFY(teams_->moveTask(team_, id, state::inReview, "dev1").ok());
        if (to == state::ready)
            finishReview(*teams_, "rev", "Verdict: no findings", true);
        QCOMPARE(get(id).state, to);
    }
    void merge(const QString& url = pullRequest) {
        host_.pullRequests.insert(
            url, {.state = QStringLiteral("merged"), .mergeCommit = mergeCommit, .error = {}});
    }
    const Team& team() { return *teams_->team(team_); }
    const Task& get(const QString& id) { return *team().task(id); }
    QList<HistoryEntry> history(const QString& id, const QString& kind) {
        QList<HistoryEntry> entries;
        for (const HistoryEntry& entry : get(id).history)
            if (entry.kind == kind)
                entries.append(entry);
        return entries;
    }
    int updates(const QString& kind) {
        int count = 0;
        for (const TeamUpdateRecord& update : journal_->teamUpdates(team_, false, 300))
            count += update.kind == kind ? 1 : 0;
        return count;
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
        open();
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
    }

    // Criteria 1 and 2: from every state that is not done, with the pull request and the merge
    // commit in the history, and nothing left waiting for the task.
    void aMergedPullRequestMakesTheTaskDoneFromEveryOpenState() {
        const QStringList states{state::todo, state::inProgress, state::inReview, state::ready,
                                 state::blocked};
        for (const QString& from : states) {
            init();
            QVERIFY(teams_->update(team_, {{"merge_policy", "user"}}, "user").ok());
            const QString id = task("Cart");
            recordPullRequest(id);
            bringTo(id, from);
            teams_->checkPullRequests(team_);
            QVERIFY2(get(id).state == from, qPrintable(from));
            merge();
            teams_->checkPullRequests(team_);
            QVERIFY2(get(id).state == state::done, qPrintable(from));
            const QList<HistoryEntry> merged = history(id, "merged");
            QCOMPARE(merged.size(), 1);
            QVERIFY(merged.first().text.contains(pullRequest));
            QVERIFY(merged.first().text.contains(mergeCommit.left(12)));
            QCOMPARE(merged.first().data.value("pull_request").toString(), pullRequest);
            QCOMPARE(merged.first().data.value("merge_commit").toString(), mergeCommit);
            QVERIFY(!team().reviewQueue.contains(id));
            QVERIFY(team().reviewing != id);
            QCOMPARE(team().openDecisions(), 0);
            QCOMPARE(updates("goal_reached"), 1);
        }
    }

    void theAnswerOfAReviewOfAMergedTaskChangesNothing() {
        const QString id = task("Cart");
        recordPullRequest(id);
        bringTo(id, state::inReview);
        QCOMPARE(team().reviewing, id);
        const QString request = team().reviewingMessage;
        merge();
        teams_->checkPullRequests(team_);
        QCOMPARE(get(id).state, state::done);
        QVERIFY(team().reviewing.isEmpty());
        teams_->reviewFinished("rev", request, "Verdict: findings\n- a bug", false);
        QCOMPARE(get(id).state, state::done);
    }

    void aMergeDecisionOfAMergedReadyTaskIsWithdrawn() {
        QVERIFY(teams_->update(team_, {{"merge_policy", "user"}}, "user").ok());
        const QString id = task("Cart");
        recordPullRequest(id);
        bringTo(id, state::ready);
        QCOMPARE(team().openDecisions(), 1);
        merge();
        teams_->checkPullRequests(team_);
        QCOMPARE(get(id).state, state::done);
        QCOMPARE(team().openDecisions(), 0);
    }

    void workMergedWhileTheDaemonWasStoppedIsReconciledAtLoad() {
        const QString id = task("Cart");
        recordPullRequest(id);
        bringTo(id, state::inProgress);
        teams_.reset();
        merge();
        open();
        QCOMPARE(get(id).state, state::done);
    }

    // Criterion 3: a branch with no commit of its own says nothing about the work.
    void anOpenPullRequestWithAnUnchangedTeamBranchStaysReady() {
        const QString id = task("Cart");
        recordPullRequest(id);
        // The work is in another repository: the team branch keeps pointing at the commit it
        // was cut from, which the integration branch contains.
        host_.heads.insert(QStringLiteral("team/%1/%2").arg(team_).arg(id), QStringLiteral("base"));
        bringTo(id, state::ready);
        QCOMPARE(get(id).headCommit, get(id).baseCommit);
        host_.merged.insert(id);
        teams_->checkMerged(team_);
        QCOMPARE(get(id).state, state::ready);
        QVERIFY(history(id, "merged").isEmpty());
    }

    void aTaskWithoutAPullRequestIsStillDoneWhenItsOwnCommitsAreMerged() {
        const QString id = task("Cart");
        bringTo(id, state::ready);
        host_.merged.insert(id);
        teams_->checkMerged(team_);
        QCOMPARE(get(id).state, state::done);
        const QList<HistoryEntry> merged = history(id, "merged");
        QCOMPARE(merged.size(), 1);
        QVERIFY(merged.first().text.contains("integration contains it"));
    }

    // Criterion 5.
    void aDependentStartsOnceItsDependencyIsMergedWithoutTheLeader() {
        const QString first = task("Api");
        const QString second = task("Cart", {first});
        recordPullRequest(first);
        QVERIFY(teams_->assignTask(team_, second, "dev2", "lead").ok());
        const TeamResult refused = teams_->moveTask(team_, second, state::inProgress, "dev2");
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error->code, QStringLiteral("dependencies_open"));
        merge();
        teams_->checkPullRequests(team_);
        QCOMPARE(get(first).state, state::done);
        QVERIFY(teams_->moveTask(team_, second, state::inProgress, "dev2").ok());
    }

    // Criterion 7: an error keeps the state and is said once, until the reason changes.
    void anErrorKeepsTheStateAndIsShownOnce() {
        const QString id = task("Cart");
        recordPullRequest(id);
        bringTo(id, state::inReview);
        host_.pullRequests.insert(pullRequest,
                                  {.state = {}, .mergeCommit = {}, .error = "offline"});
        for (int i = 0; i < 3; ++i)
            teams_->checkPullRequests(team_);
        QCOMPARE(host_.pullRequestAsks.size(), 3);
        QCOMPARE(get(id).state, state::inReview);
        QCOMPARE(history(id, "pull_request").size(), 1);
        QVERIFY(history(id, "pull_request").first().text.contains("offline"));
        QCOMPARE(updates("pull_request_unchecked"), 1);
        host_.pullRequests.insert(pullRequest,
                                  {.state = {}, .mergeCommit = {}, .error = "not signed in"});
        teams_->checkPullRequests(team_);
        QCOMPARE(history(id, "pull_request").size(), 2);
        QCOMPARE(get(id).state, state::inReview);
    }

    void withoutAWayToAskTheRemoteNothingIsDone() {
        struct Plain final : TeamHostPort {
            std::optional<TeamConversation> teamConversation(const QString&) const override {
                return std::nullopt;
            }
            QString conversationRepository(const QString&) const override { return {}; }
        } plain;
        const QString id = task("Cart");
        recordPullRequest(id);
        teams_.reset();
        TeamCoordinator teams(journal_.get(), &plain);
        teams.setPullRequestPeriod(60 * 60 * 1000, 0);
        teams.load();
        teams.checkPullRequests(team_);
        const Task* now = teams.team(team_)->task(id);
        QCOMPARE(now->state, state::todo);
        QCOMPARE(now->history.last().kind, QStringLiteral("pull_request"));
    }

    void requestsToTheRemoteAreBounded() {
        teams_->setPullRequestPeriod(60 * 60 * 1000, 60 * 1000);
        const int count = TeamCoordinator::maxPullRequestsPerCheck + 5;
        for (int i = 0; i < count; ++i) {
            const QString id = task(QStringLiteral("Task %1").arg(i));
            recordPullRequest(id, QStringLiteral("https://example.invalid/shop/pull/%1").arg(i));
        }
        // Two tasks of one pull request are one request.
        recordPullRequest(task("Same"), QStringLiteral("https://example.invalid/shop/pull/0"));
        teams_->checkPullRequests(team_);
        teams_->checkPullRequests(team_);
        teams_->checkMerged(team_);
        QCOMPARE(host_.pullRequestAsks.size(), 1);
        QCOMPARE(host_.pullRequestAsks.first().size(), TeamCoordinator::maxPullRequestsPerCheck);
        teams_->setPullRequestPeriod(60 * 60 * 1000, 0);
        teams_->checkPullRequests(team_);
        QCOMPARE(host_.pullRequestAsks.size(), 2);
        QSet<QString> asked;
        for (const QStringList& urls : host_.pullRequestAsks)
            for (const QString& url : urls)
                asked.insert(url);
        QCOMPARE(asked.size(), count);
    }

    // Criterion 6.
    void theLeaderClosesATaskByHandAndNobodyElseMay() {
        const QString id = task("Cart");
        bringTo(id, state::inProgress);
        for (const QString& actor : {"dev1", "rev", "user"}) {
            const TeamResult refused = teams_->closeTask(team_, id, mergeCommit, "In", actor);
            QVERIFY2(!refused.ok(), qPrintable(actor));
            QCOMPARE(refused.error->code, QStringLiteral("role_forbidden"));
        }
        QCOMPARE(teams_->closeTask(team_, id, "soon", "In", "lead").error->code,
                 QStringLiteral("invalid_payload"));
        QCOMPARE(teams_->closeTask(team_, id, mergeCommit, " ", "lead").error->code,
                 QStringLiteral("invalid_payload"));
        QCOMPARE(get(id).state, state::inProgress);
        QVERIFY(
            teams_->closeTask(team_, id, mergeCommit, "Merged with the api change.", "lead").ok());
        QCOMPARE(get(id).state, state::done);
        const QList<HistoryEntry> closed = history(id, "closed");
        QCOMPARE(closed.size(), 1);
        QCOMPARE(closed.first().actor, QStringLiteral("lead"));
        QCOMPARE(closed.first().data.value("by").toString(), QStringLiteral("Lead"));
        QCOMPARE(closed.first().data.value("evidence").toString(), mergeCommit);
        QVERIFY(closed.first().text.contains(mergeCommit));
        QCOMPARE(teams_->closeTask(team_, id, pullRequest, "Again", "lead").error->code,
                 QStringLiteral("invalid_transition"));
    }

    // The probe reads the tool's answer; a squash merge is a merged pull request too.
    void theProbeReadsTheToolsAnswer() {
        const auto merged = team_pull_requests::parse(
            R"({"state":"MERGED","mergeCommit":{"oid":"0123456789abcdef0123456789abcdef01234567"}})");
        QCOMPARE(merged.state, QStringLiteral("merged"));
        QCOMPARE(merged.mergeCommit, mergeCommit);
        const auto open = team_pull_requests::parse(R"({"state":"OPEN","mergeCommit":null})");
        QCOMPARE(open.state, QStringLiteral("open"));
        QVERIFY(open.mergeCommit.isEmpty());
        QCOMPARE(team_pull_requests::parse(R"({"state":"CLOSED"})").state,
                 QStringLiteral("closed"));
        const auto garbage = team_pull_requests::parse("not json");
        QVERIFY(garbage.state.isEmpty());
        QVERIFY(!garbage.error.isEmpty());
    }

    void theProbeRunsTheToolOncePerPullRequestAndReportsItsFailure() {
        QTemporaryDir bin;
        const QString tool = bin.filePath("gh");
        QFile script(tool);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(
            "#!/bin/sh\n"
            "echo \"$3\" >> \"$(dirname \"$0\")/calls\"\n"
            "case \"$3\" in\n"
            "  */pull/1) echo '{\"state\":\"MERGED\",\"mergeCommit\":{\"oid\":\"abc1234\"}}' ;;\n"
            "  *) echo 'could not resolve to a PullRequest' >&2; exit 1 ;;\n"
            "esac\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const QString one = QStringLiteral("https://example.invalid/shop/pull/1");
        const QString two = QStringLiteral("https://example.invalid/shop/pull/2");
        const auto facts = team_pull_requests::ask(bin.path(), {one, two, one}, tool);
        QCOMPARE(facts.value(one).state, QStringLiteral("merged"));
        QCOMPARE(facts.value(one).mergeCommit, QStringLiteral("abc1234"));
        QVERIFY(facts.value(two).state.isEmpty());
        QVERIFY(facts.value(two).error.contains("could not resolve"));
        QFile calls(bin.filePath("calls"));
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(calls.readAll().count('\n'), 2);
        const auto missing = team_pull_requests::ask(bin.path(), {one}, bin.filePath("none"));
        QVERIFY(missing.value(one).state.isEmpty());
        QVERIFY(!missing.value(one).error.isEmpty());
    }

    // Criterion 4: merged on the remote, with the local integration branch and the checkout
    // left as they are.
    void gitAcceptsTheRemoteIntegrationBranchAfterAFetch() {
        QTemporaryDir root;
        const QString remote = root.filePath("remote.git");
        const QString seed = root.filePath("seed");
        const QString repository = root.filePath("shop");
        QVERIFY(QDir().mkpath(seed));
        QCOMPARE(git(root.path(), {"init", "-q", "--bare", "-b", "integration", remote}),
                 QString{});
        QCOMPARE(git(seed, {"init", "-q", "-b", "integration"}), QString{});
        QCOMPARE(git(seed, {"commit", "-q", "--allow-empty", "-m", "start"}), QString{});
        QCOMPARE(git(seed, {"push", "-q", remote, "integration"}), QString{});
        QCOMPARE(git(root.path(), {"clone", "-q", remote, repository}), QString{});
        QCOMPARE(git(repository, {"checkout", "-q", "-b", "elsewhere"}), QString{});
        QCOMPARE(git(repository, {"branch", "-q", "team/1/t-1", "origin/integration"}), QString{});
        QCOMPARE(git(repository, {"worktree", "add", "-q", root.filePath("t-1"), "team/1/t-1"}),
                 QString{});
        QCOMPARE(git(root.filePath("t-1"), {"commit", "-q", "--allow-empty", "-m", "Cart"}),
                 QString{});
        const QString head = git(root.filePath("t-1"), {"rev-parse", "HEAD"});
        // The pull request is merged on the remote: the task's commit is in its integration.
        QCOMPARE(git(seed, {"fetch", "-q", repository, "team/1/t-1"}), QString{});
        QCOMPARE(git(seed, {"merge", "-q", "--no-ff", "-m", "Merge", "FETCH_HEAD"}), QString{});
        QCOMPARE(git(seed, {"push", "-q", remote, "integration"}), QString{});
        const QString localBefore = git(repository, {"rev-parse", "integration"});

        Team team;
        team.repository = repository;
        team.integrationBranch = QStringLiteral("integration");
        Task task;
        task.id = QStringLiteral("T-1");
        task.branch = QStringLiteral("team/1/t-1");
        const auto facts = team_git::facts(team, {task}, TaskQuestion::Merge);
        QCOMPARE(facts.value("T-1").head, head);
        QVERIFY(facts.value("T-1").merged.has_value());
        QVERIFY(*facts.value("T-1").merged);
        QCOMPARE(git(repository, {"rev-parse", "integration"}), localBefore);
        QCOMPARE(git(repository, {"branch", "--show-current"}), QStringLiteral("elsewhere"));
        QCOMPARE(git(repository, {"status", "--porcelain"}), QString{});
    }
};

QTEST_GUILESS_MAIN(TeamDoneTest)
#include "TeamDoneTest.moc"
