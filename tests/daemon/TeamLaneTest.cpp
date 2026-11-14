// ADR 0114 §4: lanes, the check outside a lane and lane changes. The coordinator cases use a
// fake runtime; the end-to-end case a temporary Git repository and the scripted fake
// providers. No real provider is started.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "protocol/DaemonProtocol.h"
#include "runtime/TeamGit.h"

#include <QDir>
#include <QJsonDocument>
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

QString revision(const QString& directory) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", {"rev-parse", "HEAD"});
    process.waitForFinished(15000);
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

void commitAll(const QString& directory, const QString& message) {
    git(directory, {"add", "-A"});
    git(directory,
        {"-c", "user.email=t@example.com", "-c", "user.name=T", "commit", "-q", "-m", message});
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

    // The session runtime answers after the turn that asked. The flag is set when the answer
    // comes, and a turn that ends meanwhile is checked once after it.
    void aLateAnswerFlagsTheTaskAndATurnEndedMeanwhileIsCheckedAfterIt() {
        host_.holdAnswers = true;
        host_.changed.insert(task_, {"web/cart.cpp"});
        teams_->memberTurnEnded("dev1");
        const int asked = host_.asked;
        host_.changed.insert(task_, {"web/cart.cpp", "api/routes.cpp"});
        teams_->memberTurnEnded("dev1");
        teams_->memberTurnEnded("dev1");
        QCOMPARE(host_.asked, asked);
        host_.answer();
        QVERIFY(task().outsideLane.isEmpty());
        QCOMPARE(host_.asked, asked + 1);
        host_.answer();
        QCOMPARE(task().outsideLane, (QStringList{"api/routes.cpp"}));
        QVERIFY(!teams_->checksOpen());
    }

    // The paths were compared with the integration branch the team had. After it changed the
    // answer is dropped and git is asked again.
    void anAnswerAgainstAnotherIntegrationBranchIsDroppedAndAskedAgain() {
        host_.holdAnswers = true;
        host_.changed.insert(task_, {"api/old-base.cpp"});
        teams_->memberTurnEnded("dev1");
        const int asked = host_.asked;
        QVERIFY(teams_->update(team_, {{"integration_branch", "release-other"}}, "user").ok());
        host_.changed.insert(task_, {"web/cart.cpp"});
        host_.answer();
        QVERIFY(task().outsideLane.isEmpty());
        QCOMPARE(host_.asked, asked + 1);
        host_.answer();
        QVERIFY(task().outsideLane.isEmpty());
        QVERIFY(!teams_->checksOpen());
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

    // A branch compared with a stale base once flagged 3,000 paths on each of many tasks; the
    // team's document grew past one frame and no client received the team any more.
    void aFlagOfThousandsOfPathsStaysWithinOneFrame() {
        QStringList many;
        for (int index = 0; index < 3000; ++index)
            many.append(QStringLiteral("api/module-%1/a-long-generated-file-name.ts").arg(index));
        host_.changed.insert(task_, many);
        teams_->memberTurnEnded("dev1");
        QCOMPARE(task().outsideLane.size(), maximumOutsidePaths);
        const HistoryEntry& flagged = task().history.last();
        QCOMPARE(flagged.kind, QStringLiteral("outside_lane"));
        QVERIFY(flagged.text.size() <= maximumHistoryText + 40);
        QVERIFY(flagged.text.contains(QStringLiteral("and 180 more")));
        QCOMPARE(flagged.data.value("paths").toArray().size(), maximumHistoryListItems);
        QCOMPARE(flagged.data.value("paths_total").toInt(), maximumOutsidePaths);
        QVERIFY(updates("outside_lane").first().text.contains(QStringLiteral("and 180 more")));
        QCOMPARE(updates("outside_lane").first().data.value("paths").toArray().size(),
                 maximumHistoryListItems);

        // Updates written before the bounds are bounded when team.get reads them.
        QJsonArray every;
        for (const QString& path : many)
            every.append(path);
        for (int index = 0; index < 10; ++index)
            QVERIFY(journal_->appendTeamUpdate({.teamId = team_,
                                                .at = nowIso(),
                                                .actor = "daemon",
                                                .kind = "outside_lane",
                                                .taskId = task_,
                                                .text = many.join(", "),
                                                .data = QJsonObject{{"paths", every}},
                                                .actionable = true}));
        const QByteArray listed =
            QJsonDocument(teams_->updatesJson(team_)).toJson(QJsonDocument::Compact);
        QVERIFY2(listed.size() < protocol::maximumFrameBytes / 4,
                 QByteArray::number(listed.size()).constData());

        // A document written before the bounds is bounded when it is read.
        QJsonObject stored = toJson(team());
        QJsonArray tasks = stored.value("tasks").toArray();
        QJsonObject first = tasks.first().toObject();
        QJsonArray history = first.value("history").toArray();
        QJsonArray huge;
        for (const QString& path : many)
            huge.append(path);
        for (int index = 0; index < 10; ++index)
            history.append(QJsonObject{{"at", nowIso()},
                                       {"actor", "daemon"},
                                       {"kind", "outside_lane"},
                                       {"text", many.join(", ")},
                                       {"data", QJsonObject{{"paths", huge}, {"added", huge}}}});
        first.insert("history", history);
        first.insert("outside_lane", huge);
        tasks.replace(0, first);
        stored.insert("tasks", tasks);
        QVERIFY(QJsonDocument(stored).toJson(QJsonDocument::Compact).size() >
                protocol::maximumFrameBytes);
        const Team read = teamFromJson(stored);
        const Task& old = read.tasks.first();
        QCOMPARE(old.outsideLane.size(), maximumOutsidePaths);
        QCOMPARE(old.history.last().data.value("added_total").toInt(), 3000);
        QVERIFY(old.history.last().text.size() <= maximumHistoryText + 40);
        QVERIFY(QJsonDocument(toJson(read)).toJson(QJsonDocument::Compact).size() <
                protocol::maximumFrameBytes / 4);
    }

    // Without an integration branch the base is the commit the task started from. A merge of
    // the main line into the branch brings no one else's files into the lane check.
    void withoutAnIntegrationBranchOnlyTheBranchsOwnCommitsCount() {
        QTemporaryDir root;
        const QString repository = QDir(root.path()).filePath("shop");
        QVERIFY(QDir().mkpath(repository));
        git(repository, {"init", "-q", "-b", "main"});
        write(QDir(repository).filePath("README"));
        commitAll(repository, "start");
        const QString base = revision(repository);
        const QString worktree = QDir(root.path()).filePath("task");
        git(repository, {"worktree", "add", "-q", "-b", "team/t-1", worktree});
        write(QDir(repository).filePath("api/routes.txt"));
        commitAll(repository, "someone else");
        write(QDir(worktree).filePath("web/cart.txt"));
        // A path git would quote: it is read as it is spelled (#484).
        write(QDir(worktree).filePath(QString::fromUtf8("web/gi\xe1\xbb\x8f.txt")));
        commitAll(worktree, "mine");
        git(worktree, {"-c", "user.email=t@example.com", "-c", "user.name=T", "merge", "-q",
                       "--no-edit", "main"});
        // Not committed yet but tracked: it counts. T-38: an untracked capture folder and an
        // ignored build tree never do.
        write(QDir(worktree).filePath("web/pending.txt"));
        git(worktree, {"add", "web/pending.txt"});
        {
            QFile ignore(QDir(worktree).filePath(".gitignore"));
            QVERIFY(ignore.open(QIODevice::WriteOnly));
            ignore.write("build/\n");
        }
        git(worktree, {"add", ".gitignore"});
        write(QDir(worktree).filePath("build/daemon/out.o"));
        write(QDir(worktree).filePath("captures/screen.png"));

        Team team;
        team.repository = repository;
        Task task;
        task.worktree = worktree;
        task.branch = "team/t-1";
        task.baseCommit = base;
        const auto changed = team_git::changedPaths(team, task);
        QVERIFY(changed);
        QStringList paths = *changed;
        paths.sort();
        const QString quoted = QString::fromUtf8("web/gi\xe1\xbb\x8f.txt");
        QCOMPARE(paths, (QStringList{".gitignore", "web/cart.txt", quoted, "web/pending.txt"}));
        // With an integration branch the diff is from where the branch left it: the same files.
        team.integrationBranch = "main";
        const auto against = team_git::changedPaths(team, task);
        QVERIFY(against);
        paths = *against;
        paths.sort();
        QCOMPARE(paths, (QStringList{".gitignore", "web/cart.txt", quoted, "web/pending.txt"}));
        const auto diff = team_git::taskDiff(team, task);
        QVERIFY(diff);
        QCOMPARE(diff->files, (QStringList{"web/cart.txt", quoted}));
    }

    // A text bounded when it was written is the same when the document is read again, and
    // the note still counts the characters of the original. No surrogate pair is cut in half.
    void aBoundedTextIsUnchangedWhenReadAgain() {
        const QString once = boundedText(QString(5000, u'h'));
        QVERIFY(once.size() <= maximumHistoryText);
        QVERIFY2(once.endsWith(QStringLiteral("(3040 more characters)")),
                 qPrintable(once.right(40)));
        QCOMPARE(boundedText(once), once);
        QCOMPARE(bounded({.text = once}).text, once);
        const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
        const QString cut = boundedText(QString(1959, u'h') + emoji + QString(100, u'h'));
        QVERIFY(!cut.contains(emoji.at(0)));
        QCOMPARE(cut.indexOf(QChar(0x2026)), 1959);
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
        // T-38: only tracked changes count; an untracked capture is never outside the lane.
        git(worktree, {"add", "api/routes.txt", "web/cart.txt", "tests/cart.txt"});
        write(QDir(worktree).filePath("captures/screen.png"));
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
        git(worktree, {"rm", "-q", "-f", "api/routes.txt"});
        ping(3);
        QTRY_VERIFY_WITH_TIMEOUT(outside().isEmpty(), 5000);
    }
};

QTEST_GUILESS_MAIN(TeamLaneTest)
#include "TeamLaneTest.moc"
