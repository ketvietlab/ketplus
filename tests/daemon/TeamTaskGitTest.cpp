// ADR 0114 §1: the git a task command asks for is read before the command runs. The
// coordinator cases use a fake runtime that answers when the test says so; the end-to-end case
// a temporary Git repository, a git that is slow on purpose and the scripted fake providers.
// No real provider is started.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"

#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

using namespace ketplus::daemon;
using namespace team_test;

namespace {

using Request = TeamCoordinator::TaskRequest;

void git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", arguments);
    QVERIFY(process.waitForFinished(15000));
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
}

// Puts a `git` first on PATH that waits before it runs the real one, until it is destroyed.
struct SlowGit final {
    QByteArray path = qgetenv("PATH");
    bool installed = false;

    explicit SlowGit(const QString& directory) {
        const QString real = QStandardPaths::findExecutable(QStringLiteral("git"));
        QFile script(directory + QStringLiteral("/git"));
        if (real.isEmpty() || !QDir().mkpath(directory) || !script.open(QIODevice::WriteOnly))
            return;
        script.write("#!/bin/sh\nsleep 0.3\nexec '" + real.toUtf8() + "' \"$@\"\n");
        script.close();
        installed = script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("PATH", directory.toUtf8() + ':' + path);
    }
    ~SlowGit() { qputenv("PATH", path); }
    SlowGit(const SlowGit&) = delete;
    SlowGit& operator=(const SlowGit&) = delete;
};

} // namespace

class TeamTaskGitTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};
    QString task_;

    const Task& task() { return *teams_->team(team_)->task(task_); }
    Request assignment(const QString& owner, const QString& actor = QStringLiteral("lead")) {
        return {.kind = Request::Kind::Assign,
                .teamId = team_,
                .taskId = task_,
                .actor = actor,
                .owner = owner};
    }
    Request move(const QString& to, const QString& actor) {
        return {.kind = Request::Kind::Move,
                .teamId = team_,
                .taskId = task_,
                .actor = actor,
                .to = to};
    }
    // Prepares the assignment and runs it when it may, as the session runtime does.
    void assignPrepared(const QString& owner, std::optional<TeamResult>& result) {
        teams_->prepare(assignment(owner), [this, owner, &result] {
            result = teams_->assignTask(team_, task_, owner, "lead");
        });
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
        QVERIFY(teams_->createTask(team_, {.title = "Cart", .lane = "Web"}, "lead", &task_).ok());
        host_.asked = 0;
    }

    void anAssignmentWaitsForItsWorktreeAndDoesNotAskForItAgain() {
        host_.holdAnswers = true;
        std::optional<TeamResult> result;
        assignPrepared("dev1", result);
        // Git has the question; the command has not run and the team is as it was.
        QVERIFY(!result);
        QCOMPARE(host_.held.size(), 1);
        QVERIFY(teams_->checksOpen());
        QVERIFY(task().owner.isEmpty());
        // The team answers other commands meanwhile.
        QString other;
        QVERIFY(teams_->createTask(team_, {.title = "Api", .lane = "Api"}, "lead", &other).ok());
        host_.answer();
        QVERIFY(result && result->ok());
        QCOMPARE(task().owner, QStringLiteral("dev1"));
        QCOMPARE(task().worktree, QStringLiteral("/wt/") + task_);
        QCOMPARE(task().baseCommit, QStringLiteral("base"));
        QCOMPARE(host_.made, 1);
        QVERIFY(!teams_->checksOpen());
    }

    void commandsThatWaitForOneWorktreeShareTheQuestion() {
        host_.holdAnswers = true;
        std::optional<TeamResult> first;
        std::optional<TeamResult> second;
        assignPrepared("dev1", first);
        assignPrepared("dev2", second);
        QCOMPARE(host_.held.size(), 1);
        host_.answer();
        QVERIFY(first && first->ok());
        // The second runs after the first and is refused by what the first did.
        QVERIFY(second && !second->ok());
        QCOMPARE(second->error->code, QStringLiteral("invalid_transition"));
        QCOMPARE(task().owner, QStringLiteral("dev1"));
        QCOMPARE(host_.made, 1);
    }

    void aWorktreeGitCouldNotMakeRefusesEveryCommandThatWaited() {
        host_.holdAnswers = true;
        host_.worktreeError = QStringLiteral("git could not create the worktree.");
        std::optional<TeamResult> first;
        std::optional<TeamResult> second;
        assignPrepared("dev1", first);
        assignPrepared("dev2", second);
        host_.answer();
        for (const auto& result : {first, second}) {
            QVERIFY(result && !result->ok());
            QCOMPARE(result->error->code, QStringLiteral("worktree_unavailable"));
            QCOMPARE(result->error->message, host_.worktreeError);
        }
        // Neither asked again on its own.
        QCOMPARE(host_.made, 1);
        QVERIFY(task().owner.isEmpty());
        QVERIFY(task().branch.isEmpty());
        // The answer was theirs alone: the next assignment asks git again.
        host_.holdAnswers = false;
        host_.worktreeError.clear();
        QVERIFY(teams_->assignTask(team_, task_, "dev1", "lead").ok());
        QCOMPARE(host_.made, 2);
    }

    void aWorktreeMadeForACommandRefusedMeanwhileStaysWithTheTask() {
        host_.holdAnswers = true;
        std::optional<TeamResult> result;
        assignPrepared("dev1", result);
        // While git works, the leader blocks the task: it is no longer assigned.
        QVERIFY(
            teams_->moveTask(team_, task_, state::blocked, "lead", "The API is not ready").ok());
        host_.answer();
        QVERIFY(result && !result->ok());
        QCOMPARE(result->error->code, QStringLiteral("invalid_transition"));
        QVERIFY(task().owner.isEmpty());
        // The path and the branch are taken, so the task keeps them and says why.
        QCOMPARE(task().worktree, QStringLiteral("/wt/") + task_);
        QCOMPARE(task().branch, QStringLiteral("team/%1/%2").arg(team_).arg(task_));
        QCOMPARE(task().history.constLast().kind, QStringLiteral("worktree"));
        // Its next assignment uses them instead of asking git for what is there.
        host_.holdAnswers = false;
        QVERIFY(teams_->moveTask(team_, task_, state::todo, "lead").ok());
        const int askedBefore = host_.asked;
        std::optional<TeamResult> again;
        assignPrepared("dev1", again);
        QVERIFY(again && again->ok());
        QCOMPARE(host_.asked, askedBefore);
        QCOMPARE(host_.made, 1);
    }

    void aCommandThatWouldBeRefusedOrAsksNothingRunsAtOnce() {
        host_.holdAnswers = true;
        int ran = 0;
        // Not a developer, not the leader, no such task, no such team.
        teams_->prepare(assignment("rev"), [&] { ++ran; });
        teams_->prepare(assignment("dev1", "dev2"), [&] { ++ran; });
        Request missing = assignment("dev1");
        missing.taskId = QStringLiteral("T-99");
        teams_->prepare(missing, [&] { ++ran; });
        missing = assignment("dev1");
        missing.teamId = team_ + 100;
        teams_->prepare(missing, [&] { ++ran; });
        // A handover without a reason.
        Request handover = assignment("dev1");
        handover.kind = Request::Kind::Handover;
        teams_->prepare(handover, [&] { ++ran; });
        // A move that reads nothing, and a hand to review that is not the owner's.
        teams_->prepare(move(state::blocked, "lead"), [&] { ++ran; });
        teams_->prepare(move(state::inReview, "lead"), [&] { ++ran; });
        QCOMPARE(ran, 7);
        QCOMPARE(host_.asked, 0);
        QCOMPARE(host_.made, 0);
        QVERIFY(!teams_->checksOpen());
    }

    void aHandoverMakesTheWorktreeOfATaskThatHasNone() {
        host_.holdAnswers = true;
        Request handover = assignment("dev2");
        handover.kind = Request::Kind::Handover;
        handover.reason = QStringLiteral("Dev 2 knows the cart");
        std::optional<TeamResult> result;
        teams_->prepare(handover, [&] {
            result = teams_->handoverTask(team_, task_, "dev2", handover.reason, "lead");
        });
        QVERIFY(!result);
        host_.answer();
        QVERIFY(result && result->ok());
        QCOMPARE(task().owner, QStringLiteral("dev2"));
        QCOMPARE(task().worktree, QStringLiteral("/wt/") + task_);
        QCOMPARE(host_.made, 1);
    }

    void aHandToReviewReadsTheWorktreeBeforeItRuns() {
        QVERIFY(teams_->assignTask(team_, task_, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, task_, state::inProgress, "dev1").ok());
        host_.heads.insert(task().branch, QStringLiteral("c0ffee"));
        host_.holdAnswers = true;
        const int readsBefore = host_.statusReads;
        std::optional<TeamResult> result;
        teams_->prepare(move(state::inReview, "dev1"),
                        [&] { result = teams_->moveTask(team_, task_, state::inReview, "dev1"); });
        QVERIFY(!result);
        QVERIFY(teams_->checksOpen());
        QCOMPARE(task().state, state::inProgress);
        // Only the hand to review is answered; the review's diff, asked next, stays held.
        const auto answer = host_.held.takeFirst();
        answer();
        QVERIFY(result && result->ok());
        QCOMPARE(task().state, state::inReview);
        QCOMPARE(task().headCommit, QStringLiteral("c0ffee"));
        // The worktree was read once, for the question, and not again by the command.
        QCOMPARE(host_.statusReads, readsBefore + 1);
    }

    void uncommittedWorkReadBeforehandRefusesTheHandToReview() {
        QVERIFY(teams_->assignTask(team_, task_, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, task_, state::inProgress, "dev1").ok());
        host_.dirty.insert(task().worktree);
        host_.holdAnswers = true;
        std::optional<TeamResult> result;
        teams_->prepare(move(state::inReview, "dev1"), [&] {
            // Committed since the question was asked: the command goes by what was read.
            host_.dirty.clear();
            result = teams_->moveTask(team_, task_, state::inReview, "dev1");
        });
        host_.answer();
        QVERIFY(result && !result->ok());
        QCOMPARE(result->error->code, QStringLiteral("uncommitted_work"));
        QCOMPARE(task().state, state::inProgress);
        // The answer is gone with its command: the next one reads the worktree itself.
        host_.holdAnswers = false;
        QVERIFY(teams_->moveTask(team_, task_, state::inReview, "dev1").ok());
    }
};

// The session runtime with a real repository and a git that takes its time: the commands
// answer later and the event loop runs while git works.
class TeamTaskGitProtocolTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void theEventLoopRunsWhileGitWorksForATaskCommand() {
        agent_test::Fixture fixture;
        QVERIFY(fixture.open());
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
             {"plan", QJsonObject{{"leader", QJsonObject{{"provider", "codex"}}},
                                  {"developers", QJsonObject{{"count", 1}, {"provider", "codex"}}},
                                  {"reviewer", QJsonObject{{"provider", "claude"}}}}}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(agent_test::text(created)));
        const QJsonObject team = created.value("team").toObject();
        const QString teamId = team.value("id").toString();
        const QJsonArray members = team.value("members").toArray();
        const QString leader = members.at(0).toObject().value("session_id").toString();
        const QString developer = members.at(1).toObject().value("session_id").toString();
        for (const QString& title : {"Cart", "Checkout"})
            QVERIFY(command("team.task.create", {{"team_id", teamId}, {"title", title}})
                        .value("ok")
                        .toBool());

        // The loop's pulse: a timer that only fires while the event loop runs.
        int ticks = 0;
        QTimer pulse;
        connect(&pulse, &QTimer::timeout, &pulse, [&ticks] { ++ticks; });
        pulse.start(10);
        const SlowGit slow(fixture.directory.filePath("bin"));
        QVERIFY(slow.installed);

        // The user assigns: the worktree is two slow git calls.
        QJsonObject reply;
        int ticksAtReply = 0;
        fixture.host->handleCommandDeferred(
            agent_test::request("team.task.assign",
                                {{"team_id", teamId}, {"task_id", "T-1"}, {"owner", developer}}),
            "desktop-test", [&](const QJsonObject& result) {
                reply = result;
                ticksAtReply = ticks;
            });
        QVERIFY(reply.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!reply.isEmpty(), 30000);
        QVERIFY2(reply.value("ok").toBool(), qPrintable(agent_test::text(reply)));
        QVERIFY2(ticksAtReply >= 10, qPrintable(QString::number(ticksAtReply)));
        const QString worktree = reply.value("task").toObject().value("worktree").toString();
        QVERIFY(QFileInfo(worktree).isDir());
        QVERIFY(worktree.endsWith(QStringLiteral("shop-team-%1-t-1").arg(teamId)));

        // The leader's tool assigns the other task the same way.
        reply = {};
        ticks = 0;
        fixture.host->handleAgentCommandDeferred(
            fixture.host->agentToken(leader), "agents.team_task_assign",
            {{"task_id", "T-2"}, {"owner", fixture.number(developer)}},
            [&](const QJsonObject& result) {
                reply = result;
                ticksAtReply = ticks;
            });
        QVERIFY(reply.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!reply.isEmpty(), 30000);
        QVERIFY2(reply.value("ok").toBool(), qPrintable(agent_test::text(reply)));
        QVERIFY2(ticksAtReply >= 10, qPrintable(QString::number(ticksAtReply)));
        QVERIFY(QFileInfo(reply.value("task").toObject().value("worktree").toString()).isDir());

        // The developer's tool hands its task to review: the worktree's status and the
        // branch's commit are two more.
        QVERIFY(fixture
                    .agents(developer, "agents.team_task_update",
                            {{"task_id", "T-1"}, {"state", "in_progress"}})
                    .value("ok")
                    .toBool());
        reply = {};
        ticks = 0;
        fixture.host->handleAgentCommandDeferred(
            fixture.host->agentToken(developer), "agents.team_task_update",
            {{"task_id", "T-1"}, {"state", "in_review"}}, [&](const QJsonObject& result) {
                reply = result;
                ticksAtReply = ticks;
            });
        QVERIFY(reply.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!reply.isEmpty(), 30000);
        QVERIFY2(reply.value("ok").toBool(), qPrintable(agent_test::text(reply)));
        QVERIFY2(ticksAtReply >= 10, qPrintable(QString::number(ticksAtReply)));
        QCOMPARE(reply.value("task").toObject().value("state").toString(),
                 QStringLiteral("in_review"));

        // A tool that asks git nothing, and a caller the broker refuses, answer at once.
        reply = {};
        fixture.host->handleAgentCommandDeferred(
            fixture.host->agentToken(developer), "agents.team_tasks", {},
            [&](const QJsonObject& result) { reply = result; });
        QVERIFY(reply.value("ok").toBool());
        reply = {};
        fixture.host->handleAgentCommandDeferred(
            "forged", "agents.team_task_assign", {{"task_id", "T-2"}},
            [&](const QJsonObject& result) { reply = result; });
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("authentication_failed"));
        // Git is left to finish what the review asked before the runtime goes.
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.host->teamChecksPending(), 30000);
    }
};

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    int status = 0;
    {
        TeamTaskGitTest test;
        status |= QTest::qExec(&test, argc, argv);
    }
    {
        TeamTaskGitProtocolTest test;
        status |= QTest::qExec(&test, argc, argv);
    }
    return status;
}

#include "TeamTaskGitTest.moc"
