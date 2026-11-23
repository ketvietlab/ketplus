// ADR 0114 §10: the team page's disk warning and the removal of a done task's worktree. The
// coordinator cases use a fake runtime over real folders; the git cases a temporary repository
// with a bare remote; the refusal of agents the session runtime with the scripted fake
// providers. No network and no real provider.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "TestPaths.h"
#include "agents/AgentBroker.h"
#include "agents/AgentMcpServer.h"
#include "runtime/TeamWorktreeRemoval.h"
#include "storage/LocalJournal.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>

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
    QString note_;

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
        teams_->removeWorktree(team_, id, actor, [&](const TeamResult& r, const QString& n) {
            result = r;
            note_ = n;
        });
        return result;
    }
    // T-23: the background cleanup's answer, or the refusal as {error, message}.
    QJsonObject cleanup(const QStringList& ids, const QString& actor = QStringLiteral("user")) {
        QJsonObject answer{{"error", "unanswered"}};
        teams_->cleanupWorktrees(
            team_, ids, actor, [&](const std::optional<TeamError>& error, const QJsonObject& c) {
                answer =
                    error ? QJsonObject{{"error", error->code}, {"message", error->message}} : c;
            });
        return answer;
    }
    static QStringList tasksOf(const QJsonArray& rows, const QString& key = "task_id") {
        QStringList ids;
        for (const QJsonValue& row : rows)
            ids.append(row.toObject().value(key).toString());
        return ids;
    }
    static QString stateOf(const QJsonObject& cleanup, const QString& id) {
        for (const QJsonValue& row : cleanup.value("active").toArray())
            if (row.toObject().value("task_id").toString() == id)
                return row.toObject().value("state").toString();
        return {};
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

        // Read again, the free space is asked and no worktree is walked: its size is kept.
        host_.storageFacts = {.freeBytes = 25'000'000'000LL, .sizes = {}};
        const QJsonObject again = storage();
        QCOMPARE(host_.storageAsks.size(), 2);
        QVERIFY(host_.storageAsks.last().second.isEmpty());
        QCOMPARE(again.value("free_bytes").toInteger(), 25'000'000'000LL);
        QCOMPARE(again.value("level").toString(), QString());
        QCOMPARE(
            again.value("worktrees").toArray().first().toObject().value("size_bytes").toInteger(),
            4'200'000'000LL);

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

        host_.runningIn.insert(path, QStringLiteral("“Dev 1” waits for your approval"));
        refused = remove(done);
        QCOMPARE(refused.error->code, QStringLiteral("worktree_not_removed"));
        QVERIFY2(refused.error->message.startsWith("“Dev 1” waits for your approval in "),
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
        KETPLUS_SKIP_ON_WINDOWS("the fixture links the worktree to the main checkout with "
                                "QFile::link, which makes a .lnk "
                                "shortcut on Windows, not a symbolic link");
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
        teams_->removeWorktree(team_, done, "user",
                               [&](const TeamResult& r, const QString&) { first = r; });
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
        // Nobody was in the folder, so no conversation moved.
        QCOMPARE(host_.pointed.value("dev1"), path);
    }

    // Review finding: an idle conversation still in the folder, as the developer of the last
    // task is, goes on in the main checkout; one that cannot be moved is named in words, in the
    // answer and in the history, and never said to work in the main checkout.
    void conversationsInTheWorktreeAreMovedOrNamed() {
        const QString done = taskWithWorktree("Cart");
        const QString path = get(done).worktree;
        host_.inFolder.insert(path, {QStringLiteral("dev1"), QStringLiteral("dev2")});
        host_.pointErrors.insert("dev2", QStringLiteral("Wait for startup to finish."));
        QVERIFY(remove(done).ok());
        QVERIFY(get(done).worktree.isEmpty());
        QCOMPARE(host_.pointed.value("dev1"), team().repository);
        QVERIFY(host_.pointed.value("dev2") != team().repository);
        QVERIFY2(note_.startsWith("“dev2” (Wait for startup to finish.) could not be moved"),
                 qPrintable(note_));
        QString text;
        for (const HistoryEntry& entry : get(done).history)
            if (entry.kind == "worktree_removed")
                text = entry.text;
        QVERIFY2(text.contains("“dev1” now works in the main checkout."), qPrintable(text));
        QVERIFY(!text.contains("“dev2” now works"));
        QVERIFY(text.contains(note_));
    }

    // T-23 criterion 2: a cleanup answers at once, holds each worktree as queued or removing,
    // removes them one after another and ends with a summary that names what it freed.
    void aCleanupAnswersAtOnceAndRemovesOneAfterAnother() {
        const QString first = taskWithWorktree("Cart");
        const QString second = taskWithWorktree("Search");
        const QString third = taskWithWorktree("Pay");
        host_.storageFacts = {.freeBytes = 50'000'000'000LL,
                              .sizes = {{get(first).worktree, 3'000'000'000LL},
                                        {get(second).worktree, 2'000'000'000LL},
                                        {get(third).worktree, 1'000'000'000LL}}};
        (void)storage();
        QSignalSpy changed(teams_.get(), &TeamCoordinator::storageChanged);
        host_.holdAnswers = true;

        const QJsonObject started = cleanup({first, second, third});
        // Answered before any removal ended: the first is being removed, the rest wait.
        QVERIFY2(!started.contains("error"), qPrintable(started.value("error").toString()));
        QCOMPARE(stateOf(started, first), QStringLiteral("removing"));
        QCOMPARE(stateOf(started, second), QStringLiteral("queued"));
        QCOMPARE(stateOf(started, third), QStringLiteral("queued"));
        QVERIFY(started.value("last").isNull());
        QCOMPARE(host_.removals.size(), 1);
        QVERIFY(!changed.isEmpty());
        // The rows of the team page read it from storage.
        const QJsonObject held = storage();
        QCOMPARE(held.value("cleanup").toObject().value("run"), started.value("run"));
        QStringList marks;
        for (const QJsonValue& row : held.value("worktrees").toArray())
            marks.append(row.toObject().value("task_id").toString() + "=" +
                         row.toObject().value("removal").toString());
        QCOMPARE(marks, (QStringList{first + "=removing", second + "=queued", third + "=queued"}));

        // Asked again while it runs, a worktree already in it is not queued twice.
        const QJsonObject again = cleanup({second, third});
        QCOMPARE(again.value("run"), started.value("run"));
        QCOMPARE(again.value("active").toArray().size(), 3);

        host_.answer();
        QCOMPARE(host_.removals.size(), 2);
        QCOMPARE(host_.removals.last().second, get(second).worktree);
        QVERIFY(get(first).worktree.isEmpty());
        QCOMPARE(stateOf(teams_->cleanupJson(team_), second), QStringLiteral("removing"));
        host_.answer();
        host_.answer();
        QCOMPARE(host_.removals.size(), 3);
        const QJsonObject done = teams_->cleanupJson(team_);
        QVERIFY(done.value("active").toArray().isEmpty());
        const QJsonObject last = done.value("last").toObject();
        QCOMPARE(last.value("run"), started.value("run"));
        QCOMPARE(tasksOf(last.value("removed").toArray()), (QStringList{first, second, third}));
        QVERIFY(last.value("kept").toArray().isEmpty());
        QCOMPARE(last.value("freed_bytes").toInteger(), 6'000'000'000LL);
        QVERIFY(last.value("freed_exact").toBool());
        QVERIFY(!last.value("finished_at").toString().isEmpty());
        for (const QString& id : {first, second, third})
            QVERIFY(get(id).worktree.isEmpty());
        QVERIFY(storage().value("worktrees").toArray().isEmpty());
        // A new run replaces the summary: nothing of the old one is in it.
        const QString next = taskWithWorktree("Docs");
        host_.holdAnswers = true;
        const QJsonObject second_run = cleanup({next});
        QVERIFY(second_run.value("run").toInteger() > started.value("run").toInteger());
        QVERIFY(second_run.value("last").isNull());
        host_.answer();
    }

    // T-23 criterion 1: a worktree a check refuses is kept with its reason, a conversation
    // that began in a queued one's folder is respected at its turn, and the others go on.
    void aCleanupKeepsTheWorktreesItMustNotRemove() {
        const QString removable = taskWithWorktree("Cart");
        const QString failing = taskWithWorktree("Search");
        const QString later = taskWithWorktree("Pay");
        const QString open = taskWithWorktree("Docs", false);
        host_.holdAnswers = true;
        const QJsonObject started = cleanup({removable, failing, later, open, "T-99"});
        QCOMPARE(stateOf(started, removable), QStringLiteral("removing"));
        QCOMPARE(stateOf(started, failing), QStringLiteral("queued"));
        // The open task and the unknown id are refused now, without waiting for a turn.
        QCOMPARE(stateOf(started, open), QString());
        QCOMPARE(host_.removals.size(), 1);
        // A conversation starts in the third folder, and git refuses the second (the host
        // reads its error when it is asked, which is when the first one ends).
        host_.runningIn.insert(get(later).worktree, QStringLiteral("“dev1” is running a turn"));
        host_.removalError =
            QStringLiteral("The worktree has uncommitted or untracked changes (2 files).");
        host_.answer();
        host_.answer();
        const QJsonObject last = teams_->cleanupJson(team_).value("last").toObject();
        QCOMPARE(tasksOf(last.value("removed").toArray()), QStringList{removable});
        QMap<QString, QString> kept;
        for (const QJsonValue& row : last.value("kept").toArray())
            kept.insert(row.toObject().value("task_id").toString(),
                        row.toObject().value("reason").toString());
        QStringList expected{failing, later, open, "T-99"};
        expected.sort();
        QCOMPARE(kept.keys(), expected);
        QVERIFY2(kept.value(failing).contains("uncommitted or untracked"),
                 qPrintable(kept.value(failing)));
        QVERIFY2(kept.value(later).contains("running a turn"), qPrintable(kept.value(later)));
        QVERIFY2(kept.value(open).contains("not done"), qPrintable(kept.value(open)));
        QVERIFY2(kept.value("T-99").contains("no task"), qPrintable(kept.value("T-99")));
        // Only the clean one was asked of the host, and never anything but a worktree.
        QCOMPARE(host_.removals.size(), 2);
        QVERIFY(QFileInfo(get(failing).worktree).isDir());
        QVERIFY(QFileInfo(get(later).worktree).isDir());
        QVERIFY(QFileInfo(get(open).worktree).isDir());
        QVERIFY(teams_->cleanupJson(team_).value("active").toArray().isEmpty());
    }

    // T-23: nothing to wait for when every one is refused; a bad request changes nothing.
    void aCleanupThatIsRefusedWhollyEndsAtOnce() {
        const QString open = taskWithWorktree("Docs", false);
        const QJsonObject answer = cleanup({open});
        QVERIFY(!answer.contains("error"));
        QVERIFY(answer.value("active").toArray().isEmpty());
        const QJsonObject last = answer.value("last").toObject();
        QVERIFY(last.value("removed").toArray().isEmpty());
        QCOMPARE(last.value("kept").toArray().size(), 1);
        QVERIFY(host_.removals.isEmpty());

        QCOMPARE(cleanup({}).value("error").toString(), QStringLiteral("invalid_payload"));
        QCOMPARE(cleanup({"", "  "}).value("error").toString(), QStringLiteral("invalid_payload"));
        QCOMPARE(cleanup({open}, "dev1").value("error").toString(),
                 QStringLiteral("role_forbidden"));
        QStringList many;
        for (int index = 0; index < 201; ++index)
            many.append(QStringLiteral("T-%1").arg(index));
        QCOMPARE(cleanup(many).value("error").toString(), QStringLiteral("invalid_payload"));
        QVERIFY(host_.removals.isEmpty());
        QJsonObject gone;
        teams_->cleanupWorktrees(999, {open}, "user",
                                 [&](const std::optional<TeamError>& error, const QJsonObject&) {
                                     gone = QJsonObject{{"error", error ? error->code : ""}};
                                 });
        QCOMPARE(gone.value("error").toString(), QStringLiteral("team_not_found"));
    }

    // T-23: the daemon keeps no state across a restart: a new coordinator has no run, and so no
    // row that stays "being removed".
    void aRestartedDaemonHasNoCleanupRunning() {
        const QString first = taskWithWorktree("Cart");
        const QString second = taskWithWorktree("Search");
        host_.holdAnswers = true;
        const QJsonObject started = cleanup({first, second});
        QVERIFY(!started.value("active").toArray().isEmpty());
        host_.held.clear();
        teams_.reset();
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(60 * 60 * 1000);
        teams_->setPullRequestPeriod(60 * 60 * 1000, 0);
        teams_->load();
        const QJsonObject after = teams_->cleanupJson(team_);
        QVERIFY(after.value("active").toArray().isEmpty());
        QVERIFY(after.value("last").isNull());
        QVERIFY(storage().value("worktrees").toArray().size() == 2);
        for (const QJsonValue& row : storage().value("worktrees").toArray())
            QVERIFY(!row.toObject().contains("removal"));
        // A run after the restart is numbered above the one before it, so a desktop that
        // dismissed the old summary still shows the new one.
        host_.holdAnswers = false;
        const QJsonObject next = cleanup({first});
        QVERIFY(next.value("run").toInteger() > started.value("run").toInteger());
    }

    // Review finding: a conversation that waits for an answer or an approval counts as running.
    void aWaitingConversationCountsAsRunning() {
        using storage::busy;
        QCOMPARE(busy("idle", true, false), QString());
        QCOMPARE(busy("running", false, false), QStringLiteral("is running a turn"));
        QCOMPARE(busy("waiting_for_input", false, false), QStringLiteral("waits for your answer"));
        QCOMPARE(busy("waiting_for_input", true, false), QStringLiteral("waits for your answer"));
        QCOMPARE(busy("idle", true, true), QStringLiteral("waits for your approval"));
        QCOMPARE(busy("running", false, true), QStringLiteral("waits for your approval"));
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

    // T-23 criterion 1: deleting a large worktree holds no event loop. The removal the session
    // runtime asks for runs on a pool of its own; a timer on this thread keeps firing while a
    // worktree with 20,000 ignored build files is removed, git still refuses a dirty one, and
    // nothing is forced.
    void aLargeRemovalDoesNotBlockTheEventLoop() {
        QTemporaryDir root;
        const QString remote = root.filePath("remote.git");
        const QString main = root.filePath("main");
        const QString worktree = root.filePath("main-task");
        QVERIFY(git(root.path(), {"init", "--bare", remote}) != "<failed>");
        QVERIFY(git(root.path(), {"clone", remote, main}) != "<failed>");
        QVERIFY(write(main + "/.gitignore", "build/\n"));
        QVERIFY(git(main, {"add", ".gitignore"}) != "<failed>");
        QVERIFY(git(main, {"commit", "-m", "ignore"}) != "<failed>");
        QVERIFY(git(main, {"push", "origin", "HEAD:main"}) != "<failed>");
        QVERIFY(git(main, {"worktree", "add", "-b", "team/1/T-1", worktree}) != "<failed>");
        QVERIFY(git(worktree, {"push", "origin", "team/1/T-1"}) != "<failed>");
        // A build tree: ignored, so the worktree is clean, and big enough to take a while.
        const QByteArray content(1024, 'b');
        for (int directory = 0; directory < 200; ++directory) {
            const QString folder = worktree + QStringLiteral("/build/d%1").arg(directory);
            QVERIFY(QDir().mkpath(folder));
            for (int file = 0; file < 100; ++file)
                QVERIFY(write(folder + QStringLiteral("/f%1.o").arg(file), content));
        }
        QVERIFY(team_git::sizeOnDisk(worktree) > 20'000'000);

        QThreadPool pool;
        pool.setMaxThreadCount(1);
        QObject receiver;
        QString answer = QStringLiteral("unanswered");
        bool answered = false;
        QElapsedTimer removal;
        removal.start();
        team_git::removeInBackground(pool, &receiver, main, worktree, [&](const QString& error) {
            answer = error;
            answered = true;
        });
        // Asked and returned: nothing was removed on this thread.
        QVERIFY(!answered);
        QVERIFY(removal.elapsed() < 1000);
        QElapsedTimer sinceTick;
        sinceTick.start();
        qint64 longestGap = 0;
        int ticks = 0;
        QTimer ticker;
        ticker.setInterval(5);
        connect(&ticker, &QTimer::timeout, this, [&] {
            longestGap = qMax(longestGap, sinceTick.restart());
            ++ticks;
        });
        ticker.start();
        QTRY_VERIFY_WITH_TIMEOUT(answered, 60000);
        ticker.stop();
        QCOMPARE(answer, QString());
        QVERIFY(!QFileInfo(worktree).exists());
        // The loop ran throughout; a removal on this thread would leave one gap as long as it.
        QVERIFY2(longestGap < 400,
                 qPrintable(
                     QStringLiteral("longest gap %1 ms over %2 ticks").arg(longestGap).arg(ticks)));
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

        for (const QString& operation : {"agents.team_worktree_remove", "team.worktree.remove",
                                         "agents.team_worktree_cleanup", "team.worktree.cleanup"})
            QVERIFY(!fixture.agents(agent, operation, {{"team_id", "1"}, {"task_id", "T-1"}})
                         .value("ok")
                         .toBool());

        // An agent's connection carries only agents.* operations; the commands are not one.
        for (const QString& type :
             {"team.worktree.remove", "team.worktree.cleanup", "team.storage.get"})
            QVERIFY(!AgentBroker::isOperation(type));
        QJsonObject refused;
        fixture.host->handleCommandDeferred(
            agent_test::request("team.worktree.remove", {{"team_id", "1"}, {"task_id", "T-1"}}),
            "cloud-control-v2", [&](const QJsonObject& result) { refused = result; });
        QCOMPARE(refused.value("error_code").toString(), QStringLiteral("forbidden"));
        // T-23: the background cleanup is the desktop's alone as well.
        refused = {};
        fixture.host->handleCommandDeferred(
            agent_test::request("team.worktree.cleanup",
                                {{"team_id", "1"}, {"task_ids", QJsonArray{"T-1"}}}),
            "cloud-control-v2", [&](const QJsonObject& result) { refused = result; });
        QCOMPARE(refused.value("error_code").toString(), QStringLiteral("forbidden"));

        // The desktop is answered: there is no team 1, which is not a refusal of the caller.
        QJsonObject reply;
        fixture.host->handleCommandDeferred(
            agent_test::request("team.storage.get", {{"team_id", "1"}}), "desktop-test",
            [&](const QJsonObject& result) { reply = result; });
        QTRY_VERIFY(!reply.isEmpty());
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("team_not_found"));
        reply = {};
        fixture.host->handleCommandDeferred(
            agent_test::request("team.worktree.cleanup",
                                {{"team_id", "1"}, {"task_ids", QJsonArray{"T-1"}}}),
            "desktop-test", [&](const QJsonObject& result) { reply = result; });
        QTRY_VERIFY(!reply.isEmpty());
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("team_not_found"));
    }

    // T-23: a desktop learns the background cleanup from the daemon's features.
    void theDaemonAdvertisesTheCleanup() {
        QVERIFY(protocol::features(1).contains("teams.cleanup/1"));
        QVERIFY(protocol::features(1).contains("teams.storage/1"));
    }
};

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    TeamStorageTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "TeamStorageTest.moc"
