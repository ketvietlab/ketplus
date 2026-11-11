// ADR 0114 §6: tasks, with one owner, states, criteria and history. The coordinator runs on a
// real journal with a fake runtime; the protocol case uses a temporary Git repository and the
// scripted fake providers. No real provider is started.
#include "AgentTestFixture.h"
#include "teams/TeamCoordinator.h"

#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>

using namespace ketplus::daemon;
using namespace ketplus::daemon::teams;

namespace {

class FakeHost final : public TeamHostPort {
  public:
    std::optional<TeamConversation> teamConversation(const QString& sessionId) const override {
        const auto it = conversations.constFind(sessionId);
        return it == conversations.cend() ? std::nullopt : std::optional(it.value());
    }
    std::optional<TaskWorktree> createTaskWorktree(const Team& team, const Task& task,
                                                   QString* error) override {
        (void)error;
        return TaskWorktree{.branch = QStringLiteral("team/%1/%2").arg(team.id).arg(task.id),
                            .worktree = QStringLiteral("/wt/") + task.id,
                            .baseCommit = QStringLiteral("base")};
    }
    QString pointConversationAt(const QString& sessionId, const QString& worktree) override {
        if (!pointError.isEmpty())
            return pointError;
        pointed.insert(sessionId, worktree);
        return {};
    }
    std::optional<bool> hasUncommittedWork(const QString& worktree) const override {
        return dirty.contains(worktree);
    }
    QString taskCommit(const Team&, const Task& task) const override {
        return task.branch + "@head";
    }
    QString postTeamMessage(const TeamMessage& message) override {
        messages.append(message);
        return {};
    }
    void noteInConversation(const QString& sessionId, const QJsonObject& note) override {
        notes.append({sessionId, note});
    }

    void add(const QString& id, const QString& provider = QStringLiteral("codex")) {
        conversations.insert(id, {.sessionId = id,
                                  .generation = 1,
                                  .number = id,
                                  .title = id,
                                  .provider = provider,
                                  .repository = QStringLiteral("/repo"),
                                  .idle = true});
    }

    QHash<QString, TeamConversation> conversations;
    QHash<QString, QString> pointed;
    QString pointError;
    QSet<QString> dirty;
    QList<TeamMessage> messages;
    QList<std::pair<QString, QJsonObject>> notes;
};

Team standard(const bool leaderWrites = true) {
    Team team;
    team.repository = QStringLiteral("/repo");
    team.name = QStringLiteral("Checkout");
    team.integrationBranch = QStringLiteral("integration");
    team.leaderWritesRequirements = leaderWrites;
    team.planApproved = true;
    team.members = {{.sessionId = "lead", .role = role::leader, .name = "Lead"},
                    {.sessionId = "dev1",
                     .role = role::developer,
                     .name = "Dev 1",
                     .lane = "Web",
                     .paths = {"web"}},
                    {.sessionId = "dev2",
                     .role = role::developer,
                     .name = "Dev 2",
                     .lane = "Api",
                     .paths = {"api"}},
                    {.sessionId = "rev", .role = role::reviewer, .name = "Reviewer"}};
    return team;
}

void git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", arguments);
    QVERIFY(process.waitForFinished(15000));
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
}

} // namespace

class TeamTaskTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    void reopen() {
        teams_.reset();
        journal_.reset();
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->load();
    }

    QString task(const QString& title, const QStringList& depends = {},
                 const QStringList& criteria = {"It works", "It is tested"}) {
        QString id;
        const auto result = teams_->createTask(
            team_, {.title = title, .lane = "Web", .dependsOn = depends, .criteria = criteria},
            "lead", &id);
        return result.ok() ? id : QString{};
    }

    const Task& get(const QString& id) { return *teams_->team(team_)->task(id); }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        QFile::remove(directory_.filePath("journal.sqlite3"));
        host_ = {};
        for (const QString& id : {"lead", "dev1", "dev2", "dev3"})
            host_.add(id);
        host_.add("rev", "claude");
        reopen();
        team_ = teams_->create(standard(), "user").teamId;
        QVERIFY(team_ > 0);
    }

    void idsAreNeverReusedAndOnlyTheLeaderOrTheUserCreates() {
        QCOMPARE(task("One"), QStringLiteral("T-1"));
        QCOMPARE(task("Two"), QStringLiteral("T-2"));
        QCOMPARE(
            teams_->createTask(team_, {.title = "Mine"}, "dev1").error.value_or(TeamError{}).code,
            QStringLiteral("role_forbidden"));
        QCOMPARE(
            teams_->createTask(team_, {.title = "Theirs"}, "rev").error.value_or(TeamError{}).code,
            QStringLiteral("role_forbidden"));
        QVERIFY(teams_->createTask(team_, {.title = "From the user"}, "user").ok());
        QCOMPARE(teams_->team(team_)->tasks.size(), 3);
        QCOMPARE(teams_->createTask(team_, {.title = "Waits", .dependsOn = {"T-9"}}, "lead")
                     .error.value_or(TeamError{})
                     .code,
                 QStringLiteral("task_not_found"));
    }

    void theStateTable() {
        const QString id = task("Cart");
        // Nobody starts an unassigned task; ready and done are the daemon's.
        QCOMPARE(
            teams_->moveTask(team_, id, state::inProgress, "dev1").error.value_or(TeamError{}).code,
            QStringLiteral("not_owner"));
        QCOMPARE(teams_->moveTask(team_, id, state::ready, "lead").error.value_or(TeamError{}).code,
                 QStringLiteral("role_forbidden"));
        QCOMPARE(teams_->moveTask(team_, id, state::done, "user").error.value_or(TeamError{}).code,
                 QStringLiteral("role_forbidden"));
        QCOMPARE(teams_->assignTask(team_, id, "dev1", "dev2").error.value_or(TeamError{}).code,
                 QStringLiteral("role_forbidden"));
        QCOMPARE(teams_->assignTask(team_, id, "rev", "lead").error.value_or(TeamError{}).code,
                 QStringLiteral("invalid_payload"));
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QCOMPARE(get(id).owner, QStringLiteral("dev1"));
        QCOMPARE(get(id).branch, QStringLiteral("team/%1/T-1").arg(team_));
        QCOMPARE(host_.pointed.value("dev1"), QStringLiteral("/wt/T-1"));
        QCOMPARE(host_.messages.size(), 1);
        QCOMPARE(host_.messages.first().kind, QStringLiteral("assignment"));
        QCOMPARE(host_.messages.first().toSession, QStringLiteral("dev1"));
        QVERIFY(host_.messages.first().body.contains("It is tested"));
        QCOMPARE(teams_->assignTask(team_, id, "dev2", "lead").error.value_or(TeamError{}).code,
                 QStringLiteral("invalid_transition"));
        // todo → in_review is not a move; another member cannot start it.
        QCOMPARE(
            teams_->moveTask(team_, id, state::inReview, "dev1").error.value_or(TeamError{}).code,
            QStringLiteral("invalid_transition"));
        QCOMPARE(
            teams_->moveTask(team_, id, state::inProgress, "dev2").error.value_or(TeamError{}).code,
            QStringLiteral("not_owner"));
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        // One task in progress per developer.
        const QString second = task("Second");
        QVERIFY(teams_->assignTask(team_, second, "dev1", "lead").ok());
        QCOMPARE(teams_->moveTask(team_, second, state::inProgress, "dev1")
                     .error.value_or(TeamError{})
                     .code,
                 QStringLiteral("invalid_transition"));
        // Blocking needs the question; it goes to the leader as actionable.
        QCOMPARE(
            teams_->moveTask(team_, id, state::blocked, "dev1").error.value_or(TeamError{}).code,
            QStringLiteral("invalid_payload"));
        QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", "Which API?").ok());
        QCOMPARE(get(id).question, QStringLiteral("Which API?"));
        QCOMPARE(get(id).resumeState, state::inProgress);
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        // Uncommitted work is not reviewed.
        host_.dirty.insert("/wt/T-1");
        const auto dirty = teams_->moveTask(team_, id, state::inReview, "dev1");
        QCOMPARE(dirty.error.value_or(TeamError{}).code, QStringLiteral("uncommitted_work"));
        QVERIFY(dirty.error->message.contains("Commit your work in /wt/T-1"));
        host_.dirty.clear();
        QVERIFY(teams_->moveTask(team_, id, state::inReview, "dev1").ok());
        QCOMPARE(get(id).headCommit, get(id).branch + QStringLiteral("@head"));
        QCOMPARE(
            teams_->moveTask(team_, id, state::inProgress, "dev1").error.value_or(TeamError{}).code,
            QStringLiteral("invalid_transition"));
        QVERIFY(teams_->moveTask(team_, id, state::ready, "daemon").ok());
        QVERIFY(teams_->moveTask(team_, id, state::done, "daemon").ok());
        QCOMPARE(teams_->moveTask(team_, id, state::todo, "lead").error.value_or(TeamError{}).code,
                 QStringLiteral("invalid_transition"));
        // The leader can take a task back to To do, which unassigns it.
        QVERIFY(teams_->moveTask(team_, second, state::todo, "lead").ok());
        QVERIFY(get(second).owner.isEmpty());
        QCOMPARE(
            teams_->moveTask(team_, second, "archived", "lead").error.value_or(TeamError{}).code,
            QStringLiteral("invalid_payload"));
        // Ready, blocked and done are actionable updates for the digest.
        int actionable = 0;
        for (const auto& update : journal_->teamUpdates(team_, true, 200))
            actionable += update.actionable ? 1 : 0;
        QVERIFY(actionable >= 3);
    }

    void dependenciesMustBeDoneFirst() {
        const QString first = task("Schema");
        const QString second = task("Endpoint", {first});
        QVERIFY(teams_->assignTask(team_, second, "dev2", "lead").ok());
        const auto refused = teams_->moveTask(team_, second, state::inProgress, "dev2");
        QCOMPARE(refused.error.value_or(TeamError{}).code, QStringLiteral("dependencies_open"));
        // Tasks cannot wait for each other in a circle, however long.
        const QString third = task("Third", {second});
        QCOMPARE(teams_->updateTask(team_, first, {{"depends_on", QJsonArray{third}}}, "lead")
                     .error.value_or(TeamError{})
                     .code,
                 QStringLiteral("dependency_cycle"));
        QVERIFY(get(first).dependsOn.isEmpty());
        QVERIFY(refused.error->message.contains(first));
        QVERIFY(teams_->assignTask(team_, first, "dev1", "lead").ok());
        for (const QString& next : {state::inProgress})
            QVERIFY(teams_->moveTask(team_, first, next, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, first, state::inReview, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, first, state::ready, "daemon").ok());
        QVERIFY(teams_->moveTask(team_, first, state::done, "daemon").ok());
        QVERIFY(teams_->moveTask(team_, second, state::inProgress, "dev2").ok());
    }

    void criteriaTextIsTheLeadersAndTheMarkIsTheOwners() {
        const QString id = task("Cart");
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QCOMPARE(
            teams_->setCriteria(team_, id, {"Changed"}, "dev1").error.value_or(TeamError{}).code,
            QStringLiteral("role_forbidden"));
        QCOMPARE(
            teams_->setCriteria(team_, id, {"Changed"}, "rev").error.value_or(TeamError{}).code,
            QStringLiteral("role_forbidden"));
        QVERIFY(teams_
                    ->updateTask(team_, id,
                                 {{"met", QJsonArray{QJsonObject{{"index", 0}, {"met", true}}}}},
                                 "dev1")
                    .ok());
        QCOMPARE(teams_
                     ->updateTask(team_, id,
                                  {{"met", QJsonArray{QJsonObject{{"index", 1}, {"met", true}}}}},
                                  "dev2")
                     .error.value_or(TeamError{})
                     .code,
                 QStringLiteral("not_owner"));
        QCOMPARE(teams_->updateTask(team_, id, {{"title", "Mine now"}}, "dev1")
                     .error.value_or(TeamError{})
                     .code,
                 QStringLiteral("role_forbidden"));
        QVERIFY(teams_->setCriteria(team_, id, {"It works", "It is fast"}, "lead").ok());
        const Task& changed = get(id);
        QCOMPARE(changed.criteria.size(), 2);
        QVERIFY(changed.criteria.at(0).met); // unchanged text keeps its mark
        QVERIFY(!changed.criteria.at(1).met);
        const HistoryEntry& entry = changed.history.last();
        QCOMPARE(entry.kind, QStringLiteral("criteria"));
        QCOMPARE(entry.data.value("from").toArray().at(1).toString(),
                 QStringLiteral("It is tested"));
        QCOMPARE(entry.data.value("to").toArray().at(1).toString(), QStringLiteral("It is fast"));
        // A note and a pull request come from the owner.
        QVERIFY(
            teams_->updateTask(team_, id, {{"note", "Half done"}, {"pull_request", "#12"}}, "dev1")
                .ok());
        QCOMPARE(get(id).pullRequest, QStringLiteral("#12"));
    }

    void requirementsWrittenElsewhereAreTheUsers() {
        QVERIFY(!teams_->remove(team_));
        Team team = standard(false);
        team_ = teams_->create(team, "user").teamId;
        QCOMPARE(teams_->createTask(team_, {.title = "T", .criteria = {"x"}}, "lead")
                     .error.value_or(TeamError{})
                     .code,
                 QStringLiteral("role_forbidden"));
        QString id;
        QVERIFY(teams_->createTask(team_, {.title = "T"}, "lead", &id).ok());
        QCOMPARE(teams_->setCriteria(team_, id, {"x"}, "lead").error.value_or(TeamError{}).code,
                 QStringLiteral("role_forbidden"));
        QVERIFY(teams_->setCriteria(team_, id, {"x"}, "user").ok());
    }

    void handoverMovesTheWorktreeAndKeepsOneOwner() {
        const QString id = task("Cart");
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QCOMPARE(
            teams_->handoverTask(team_, id, "dev2", "", "lead").error.value_or(TeamError{}).code,
            QStringLiteral("invalid_payload"));
        QCOMPARE(teams_->handoverTask(team_, id, "dev2", "Busy", "dev1")
                     .error.value_or(TeamError{})
                     .code,
                 QStringLiteral("role_forbidden"));
        QCOMPARE(get(id).owner, QStringLiteral("dev1"));
        host_.messages.clear();
        QVERIFY(teams_->handoverTask(team_, id, "dev2", "Dev 1 is on the API", "lead").ok());
        const Task& moved = get(id);
        QCOMPARE(moved.owner, QStringLiteral("dev2"));
        QCOMPARE(moved.branch, QStringLiteral("team/%1/T-1").arg(team_));
        QCOMPARE(moved.state, state::inProgress);
        QCOMPARE(host_.pointed.value("dev2"), QStringLiteral("/wt/T-1"));
        // The previous owner leaves the worktree that is not its own any more.
        QCOMPARE(host_.pointed.value("dev1"), QStringLiteral("/repo"));
        QCOMPARE(host_.messages.size(), 1);
        QVERIFY(host_.messages.first().body.contains("handed over"));
        QVERIFY(host_.messages.first().body.contains("History:"));
        QCOMPARE(host_.notes.size(), 1);
        QCOMPARE(host_.notes.first().first, QStringLiteral("dev1"));
        QCOMPARE(moved.history.last().data.value("reason").toString(),
                 QStringLiteral("Dev 1 is on the API"));
    }

    void assignmentIsDeliveredWhenTheConversationCannotBeMoved() {
        // Several tasks, so that recording the failure reallocates the stored list.
        for (int index = 0; index < 8; ++index)
            (void)task(QStringLiteral("Filler %1").arg(index));
        const QString id = task("Cart");
        host_.pointError = QStringLiteral("The conversation is running a turn.");
        host_.messages.clear();
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QCOMPARE(host_.messages.size(), 1);
        QCOMPARE(host_.messages.first().toSession, QStringLiteral("dev1"));
        QCOMPARE(host_.messages.first().taskId, id);
        QVERIFY(host_.messages.first().body.contains(QStringLiteral("\"Cart\"")));
        QVERIFY(get(id).history.last().text.contains("was not moved"));
        QVERIFY(!host_.pointed.contains("dev1"));
    }

    void removingTheOwnerUnassignsAndKeepsTheBranchAndHistory() {
        const QString id = task("Cart");
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        const auto historyBefore = get(id).history.size();
        QVERIFY(teams_->removeMember(team_, "dev1", "user").ok());
        QVERIFY(get(id).owner.isEmpty());
        QCOMPARE(get(id).state, state::todo);
        QCOMPARE(get(id).branch, QStringLiteral("team/%1/T-1").arg(team_));
        QVERIFY(get(id).history.size() > historyBefore);
        // Everything survives a restart.
        reopen();
        QCOMPARE(get(id).history.size(), teams_->team(team_)->task(id)->history.size());
        QCOMPARE(get(id).criteria.size(), 2);
        QCOMPARE(teams_->team(team_)->nextTask, 2);
    }
};

// teams.tasks/1 with a real repository: the worktree, the branch and the assignment message.
class TeamTaskProtocolTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void assignmentCreatesTheTaskWorktreeAndMessagesTheOwner() {
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
        const QString developer =
            team.value("members").toArray().at(1).toObject().value("session_id").toString();
        QJsonObject result = command(
            "team.task.create",
            {{"team_id", teamId}, {"title", "Cart"}, {"criteria", QJsonArray{"Adds an item"}}});
        QVERIFY2(result.value("ok").toBool(), qPrintable(agent_test::text(result)));
        QCOMPARE(result.value("task").toObject().value("id").toString(), QStringLiteral("T-1"));
        result = command("team.task.assign",
                         {{"team_id", teamId}, {"task_id", "T-1"}, {"owner", developer}});
        QVERIFY2(result.value("ok").toBool(), qPrintable(agent_test::text(result)));
        const QJsonObject task = result.value("task").toObject();
        QCOMPARE(task.value("branch").toString(), QStringLiteral("team/%1/t-1").arg(teamId));
        const QString worktree = task.value("worktree").toString();
        QVERIFY(QFileInfo(worktree).isDir());
        QVERIFY(worktree.endsWith(QStringLiteral("shop-team-%1-t-1").arg(teamId)));
        // The owner now works in the task's worktree and receives the assignment.
        const QJsonArray listed = command("session.list", {}).value("sessions").toArray();
        QString path;
        for (const auto& value : listed)
            if (value.toObject().value("session_id").toString() == developer)
                path = value.toObject().value("worktree_path").toString();
        QCOMPARE(QFileInfo(path).canonicalFilePath(), QFileInfo(worktree).canonicalFilePath());
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.journal->agentMessagesOf(developer, 1, 10).isEmpty(),
                                 5000);
        const auto message = fixture.journal->agentMessagesOf(developer, 1, 10).first();
        QCOMPARE(message.kind, QStringLiteral("assignment"));
        QVERIFY(message.body.contains("Adds an item"));
        // Uncommitted work is refused with the worktree named.
        QVERIFY(command("team.task.move",
                        {{"team_id", teamId}, {"task_id", "T-1"}, {"state", "in_progress"}})
                    .value("error_code") == QStringLiteral("not_owner"));
        QFile file(QDir(worktree).filePath("cart.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("cart");
        file.close();
        QCOMPARE(command("team.task.get", {{"team_id", teamId}, {"task_id", "T-1"}})
                     .value("task")
                     .toObject()
                     .value("history")
                     .toArray()
                     .size(),
                 2);
        QVERIFY(protocol::features(1).contains("teams.tasks/1"));
    }
};

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    int status = 0;
    {
        TeamTaskTest test;
        status |= QTest::qExec(&test, argc, argv);
    }
    {
        TeamTaskProtocolTest test;
        status |= QTest::qExec(&test, argc, argv);
    }
    return status;
}

#include "TeamTaskTest.moc"
