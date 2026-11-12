// ADR 0114 §10: the team page's disk warning and the removal of a done task's worktree. The
// coordinator cases use a fake runtime over real folders; the git cases a temporary repository
// with a bare remote; the refusal of agents the session runtime with the scripted fake
// providers. No network and no real provider.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "agents/AgentBroker.h"
#include "agents/AgentMcpServer.h"
#include "runtime/TeamWorktreeRemoval.h"
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
    process.start("git", QStringList{"-c", "user.email=t@example.com", "-c", "user.name=T", "-c",
                                     "init.defaultBranch=main"} +
                             arguments);
    if (!process.waitForFinished(15000) || process.exitCode() != 0) {
        qWarning("%s", process.readAllStandardError().constData());
        return QStringLiteral("<failed>");
    }
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

bool write(const QString& path, const QByteArray& text) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(text) == text.size();
}

} // namespace

class TeamStorageTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    // A task of dev1's with its worktree on disk, closed by the leader unless `done` is false.
    QString taskWithWorktree(const QString& title, const bool done = true) {
        QString id;
        (void)teams_->createTask(team_, {.title = title, .lane = "Web"}, "lead", &id);
        [&] {
            QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
            QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
            QVERIFY(QDir().mkpath(get(id).worktree));
            QVERIFY(write(get(id).worktree + "/file.txt", QByteArray(8192, 'x')));
            if (done)
                QVERIFY(teams_->closeTask(team_, id, evidence, "Merged.", "lead").ok());
        }();
        return id;
    }
    const Team& team() { return *teams_->team(team_); }
    const Task& get(const QString& id) { return *team().task(id); }
    QJsonObject storage() {
        QJsonObject answer;
        teams_->storage(team_, [&](const std::optional<TeamError>& error, const QJsonObject& s) {
            answer = error ? QJsonObject{{"error", error->code}} : s;
        });
        return answer;
    }
    TeamResult remove(const QString& id, const QString& actor = QStringLiteral("user")) {
        TeamResult result{.error = TeamError{"unanswered", {}}, .teamId = team_};
        teams_->removeWorktree(team_, id, actor, [&](const TeamResult& r) { result = r; });
        return result;
    }
    int historyOf(const QString& id, const QString& kind) {
        int count = 0;
        for (const HistoryEntry& entry : get(id).history)
            count += entry.kind == kind ? 1 : 0;
        return count;
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
        host_.repository = directory_.filePath("repo");
        for (const QString& id : {"lead", "dev1", "dev2"})
            host_.add(id);
        host_.add("rev", "claude");
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(60 * 60 * 1000);
        teams_->setPullRequestPeriod(60 * 60 * 1000, 0);
        teams_->load();
        Team made = standardTeam();
        made.repository = host_.repository;
        QVERIFY(QDir().mkpath(made.repository));
        team_ = teams_->create(made, "user").teamId;
        QVERIFY(team_ > 0);
    }

    // Criterion 1: two levels, each below its constant; unknown space warns of nothing.
    void theLevelsAreBelowTheirThresholds() {
        using namespace storage;
        QCOMPARE(warnBytes, 20LL * 1000 * 1000 * 1000);
        QCOMPARE(stopBytes, 12LL * 1000 * 1000 * 1000);
        QCOMPARE(level(std::nullopt), QString());
        QCOMPARE(level(warnBytes), QString());
        QCOMPARE(level(warnBytes - 1), QStringLiteral("low"));
        QCOMPARE(level(stopBytes), QStringLiteral("low"));
        QCOMPARE(level(stopBytes - 1), QStringLiteral("stop"));
        QCOMPARE(level(0), QStringLiteral("stop"));
    }

    // Criteria 1 and 2: the volume of the repository is measured, with each done task's
    // worktree that is still on disk; nothing is paused or removed.
    void storageNamesTheFreeSpaceAndTheDoneWorktrees() {
        const QString done = taskWithWorktree("Cart");
        const QString gone = taskWithWorktree("Gone");
        const QString open = taskWithWorktree("Search", false);
        QVERIFY(QDir(get(gone).worktree).removeRecursively());
        host_.storageFacts = {.freeBytes = 9'600'000'000LL,
                              .sizes = {{get(done).worktree, 4'200'000'000LL}}};
        const QJsonObject answer = storage();
        QCOMPARE(host_.storageAsks.size(), 1);
        QCOMPARE(host_.storageAsks.first().first, team().repository);
        QCOMPARE(host_.storageAsks.first().second, QStringList{get(done).worktree});
        QCOMPARE(answer.value("free_bytes").toInteger(), 9'600'000'000LL);
        QCOMPARE(answer.value("level").toString(), QStringLiteral("stop"));
        QCOMPARE(answer.value("warn_bytes").toInteger(), storage::warnBytes);
        QCOMPARE(answer.value("stop_bytes").toInteger(), storage::stopBytes);
        const QJsonArray worktrees = answer.value("worktrees").toArray();
        QCOMPARE(worktrees.size(), 1);
        const QJsonObject entry = worktrees.first().toObject();
        QCOMPARE(entry.value("task_id").toString(), done);
        QCOMPARE(entry.value("title").toString(), QStringLiteral("Cart"));
        QCOMPARE(entry.value("path").toString(), get(done).worktree);
        QCOMPARE(entry.value("size_bytes").toInteger(), 4'200'000'000LL);
        QVERIFY(host_.removals.isEmpty());
        QVERIFY(QFileInfo(get(open).worktree).isDir());

        host_.storageFacts = {};
        QVERIFY(storage().value("free_bytes").isNull());
        QCOMPARE(storage().value("level").toString(), QString());
    }

    // Criterion 3: each refusal says why in words and changes nothing.
    void aRefusedRemovalChangesNothing() {
        const QString done = taskWithWorktree("Cart");
        const QString open = taskWithWorktree("Search", false);
        const QString path = get(done).worktree;
        const qint64 revision = team().revision;

        TeamResult refused = remove(open);
        QCOMPARE(refused.error->code, QStringLiteral("worktree_not_removed"));
        QVERIFY(refused.error->message.contains("not done"));

        host_.runningIn.insert(path, QStringLiteral("Dev 1"));
        refused = remove(done);
        QCOMPARE(refused.error->code, QStringLiteral("worktree_not_removed"));
        QVERIFY2(refused.error->message.contains("running a turn"),
                 qPrintable(refused.error->message));
        host_.runningIn.clear();

        QCOMPARE(remove("T-99").error->code, QStringLiteral("task_not_found"));
        QVERIFY(host_.removals.isEmpty());
        QCOMPARE(team().revision, revision);
        QCOMPARE(get(done).worktree, path);
        QVERIFY(QFileInfo(path).isDir());
        QCOMPARE(historyOf(done, "worktree_removed"), 0);
    }

    // Criterion 3: the main checkout is never offered nor removed, by whatever path it is named.
    void theMainCheckoutIsRefused() {
        QString id;
        (void)teams_->createTask(team_, {.title = "Cart", .lane = "Web"}, "lead", &id);
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(QDir().mkpath(directory_.filePath("wt")));
        QVERIFY(QFile::link(team().repository, get(id).worktree));
        QVERIFY(teams_->closeTask(team_, id, evidence, "Merged.", "lead").ok());
        QVERIFY(QFileInfo(get(id).worktree).isDir());
        QVERIFY(storage().value("worktrees").toArray().isEmpty());
        const TeamResult refused = remove(id);
        QCOMPARE(refused.error->code, QStringLiteral("worktree_not_removed"));
        QVERIFY(refused.error->message.contains("main checkout"));
        QVERIFY(host_.removals.isEmpty());
        QVERIFY(QFileInfo(team().repository).isDir());
    }

    // Criterion 5: an agent, even the leader, is refused by the coordinator as well.
    void anAgentActorIsRefused() {
        const QString done = taskWithWorktree("Cart");
        for (const QString& actor : {"lead", "dev1", "rev"}) {
            const TeamResult refused = remove(done, actor);
            QCOMPARE(refused.error->code, QStringLiteral("role_forbidden"));
        }
        QVERIFY(host_.removals.isEmpty());
        QVERIFY(QFileInfo(get(done).worktree).isDir());
    }

    // Criterion 4: a failure is shown in words and the task keeps its worktree entry.
    void aFailedRemovalKeepsTheWorktreeEntry() {
        const QString done = taskWithWorktree("Cart");
        const QString path = get(done).worktree;
        host_.removalError = QStringLiteral("git worktree remove failed: locked");
        const TeamResult failed = remove(done);
        QCOMPARE(failed.error->code, QStringLiteral("worktree_not_removed"));
        QCOMPARE(failed.error->message, host_.removalError);
        QCOMPARE(host_.removals.size(), 1);
        QCOMPARE(host_.removals.first(), std::pair(team().repository, path));
        QCOMPARE(get(done).worktree, path);
        QCOMPARE(historyOf(done, "worktree_removed"), 0);
    }

    // Criterion 4: a removal clears the entry, says so in the history and is not run twice.
    void aConfirmedRemovalClearsTheEntry() {
        const QString done = taskWithWorktree("Cart");
        const QString path = get(done).worktree;
        host_.holdAnswers = true;
        TeamResult first{.error = TeamError{"unanswered", {}}, .teamId = team_};
        teams_->removeWorktree(team_, done, "user", [&](const TeamResult& r) { first = r; });
        QCOMPARE(first.error->code, QStringLiteral("unanswered"));
        const TeamResult again = remove(done);
        QVERIFY2(again.error && again.error->message.contains("being removed"),
                 qPrintable(again.error ? again.error->message : QString()));
        host_.answer();
        QVERIFY(first.ok());
        QCOMPARE(host_.removals.size(), 1);
        QVERIFY(get(done).worktree.isEmpty());
        QCOMPARE(get(done).state, state::done);
        QCOMPARE(historyOf(done, "worktree_removed"), 1);
        QVERIFY(!QFileInfo(path).exists());
        QVERIFY(storage().value("worktrees").toArray().isEmpty());
    }

    // Criteria 3 and 4 against git: the checks, then remove without force and prune.
    void gitRemovesOnlyACleanPushedWorktree() {
        QTemporaryDir root;
        const QString remote = root.filePath("remote.git");
        const QString main = root.filePath("main");
        const QString worktree = root.filePath("main-task");
        QVERIFY(git(root.path(), {"init", "--bare", remote}) != "<failed>");
        QVERIFY(git(root.path(), {"clone", remote, main}) != "<failed>");
        QVERIFY(write(main + "/a.txt", "a\n"));
        QVERIFY(git(main, {"add", "a.txt"}) != "<failed>");
        QVERIFY(git(main, {"commit", "-m", "a"}) != "<failed>");
        QVERIFY(git(main, {"push", "origin", "HEAD:main"}) != "<failed>");
        QVERIFY(git(main, {"worktree", "add", "-b", "team/1/T-1", worktree}) != "<failed>");

        QVERIFY(team_git::removeWorktree(main, main).contains("main checkout"));

        QVERIFY(write(worktree + "/new.txt", "untracked\n"));
        QString refused = team_git::removeWorktree(main, worktree);
        QVERIFY2(refused.contains("uncommitted or untracked changes (1 file)"),
                 qPrintable(refused));
        QVERIFY(QFileInfo(worktree + "/new.txt").exists());

        QVERIFY(git(worktree, {"add", "new.txt"}) != "<failed>");
        QVERIFY(git(worktree, {"commit", "-m", "new"}) != "<failed>");
        refused = team_git::removeWorktree(main, worktree);
        QVERIFY2(refused.contains("1 commit on no remote branch"), qPrintable(refused));
        QVERIFY(QFileInfo(worktree).isDir());

        QVERIFY(git(worktree, {"push", "origin", "team/1/T-1"}) != "<failed>");
        QVERIFY(git(main, {"worktree", "lock", worktree}) != "<failed>");
        refused = team_git::removeWorktree(main, worktree);
        QVERIFY2(refused.startsWith("git worktree remove failed"), qPrintable(refused));
        QVERIFY(QFileInfo(worktree).isDir());

        QVERIFY(git(main, {"worktree", "unlock", worktree}) != "<failed>");
        QVERIFY(team_git::sizeOnDisk(worktree) > 0);
        QCOMPARE(team_git::removeWorktree(main, worktree), QString());
        QVERIFY(!QFileInfo(worktree).exists());
        QVERIFY(!git(main, {"worktree", "list"}).contains("main-task"));
        // The branch and its commits stay.
        QVERIFY(git(main, {"rev-parse", "--verify", "team/1/T-1"}) != "<failed>");
    }

    // Criterion 5: the daemon serves the commands only to the desktop user. An agent has no
    // tool for them, and neither its own channel nor another client reaches them.
    void anAgentCannotRemoveAWorktree() {
        agent_test::Fixture fixture;
        QVERIFY(fixture.open());
        const QString agent = fixture.start("codex", "Hello");
        QVERIFY(!agent.isEmpty());

        const QJsonObject tools = agent_mcp::handleMessage(
            {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
            [](const QString& operation, const QJsonObject&) {
                return operation == "agents.whoami"
                           ? QJsonObject{{"ok", true}, {"team", QJsonObject{{"role", "leader"}}}}
                           : QJsonObject{};
            });
        const QJsonArray listed = tools.value("result").toObject().value("tools").toArray();
        QVERIFY(!listed.isEmpty());
        for (const QJsonValue& tool : listed)
            QVERIFY(!tool.toObject().value("name").toString().contains("worktree"));

        for (const QString& operation : {"agents.team_worktree_remove", "team.worktree.remove"})
            QVERIFY(!fixture.agents(agent, operation, {{"team_id", "1"}, {"task_id", "T-1"}})
                         .value("ok")
                         .toBool());

        // An agent's connection carries only agents.* operations; the commands are not one.
        for (const QString& type : {"team.worktree.remove", "team.storage.get"})
            QVERIFY(!AgentBroker::isOperation(type));
        QJsonObject refused;
        fixture.host->handleCommandDeferred(
            agent_test::request("team.worktree.remove", {{"team_id", "1"}, {"task_id", "T-1"}}),
            "cloud-control-v2", [&](const QJsonObject& result) { refused = result; });
        QCOMPARE(refused.value("error_code").toString(), QStringLiteral("forbidden"));

        // The desktop is answered: there is no team 1, which is not a refusal of the caller.
        QJsonObject reply;
        fixture.host->handleCommandDeferred(
            agent_test::request("team.storage.get", {{"team_id", "1"}}), "desktop-test",
            [&](const QJsonObject& result) { reply = result; });
        QTRY_VERIFY(!reply.isEmpty());
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("team_not_found"));
    }
};

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    TeamStorageTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "TeamStorageTest.moc"
