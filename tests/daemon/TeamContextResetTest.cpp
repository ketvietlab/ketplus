// T-1: when a turn the team starts gets a fresh native context, the team's context reset
// settings, and the briefing the fresh context begins with. The coordinator cases use a fake
// runtime; the protocol cases the scripted fake providers. No real provider is started.
#include "teams/TeamContextReset.h"
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "storage/LocalJournal.h"

#include <QDir>
#include <QStandardPaths>
#include <QTest>

using namespace ketplus::daemon;
using namespace team_test;

namespace {

std::optional<StoredSession> storedSession(const LocalJournal& journal, const QString& id) {
    for (const StoredSession& session : journal.sessions())
        if (session.id == id)
            return session;
    return std::nullopt;
}

QJsonObject plan() {
    return {{"leader", QJsonObject{{"provider", "claude"}}},
            {"developers",
             QJsonObject{{"count", 1},
                         {"provider", "codex"},
                         {"members", QJsonArray{QJsonObject{{"paths", QJsonArray{"web"}}}}}}},
            {"reviewer", QJsonObject{{"provider", "claude"}}}};
}

} // namespace

class TeamContextResetTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};
    QString first_;
    QString second_;

    const Team& team() { return *teams_->team(team_); }
    QString reason(const QString& session, const QString& hint, const QString& thread,
                   const qint64 turn) {
        const auto planned = teams_->planContextReset(session, hint, thread, turn);
        return planned ? planned->reason : QString{};
    }
    void load() {
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(60 * 60 * 1000);
        teams_->load();
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        teams_.reset();
        journal_.reset();
        QFile::remove(directory_.filePath("journal.sqlite3"));
        host_ = {};
        for (const QString& id : QStringList{"lead", "dev1", "dev2"})
            host_.add(id);
        host_.add("rev", "claude");
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        load();
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
        (void)teams_->createTask(team_,
                                 TaskDraft{.title = "First", .lane = "Web", .criteria = {"Works"}},
                                 "lead", &first_);
        (void)teams_->createTask(team_, {.title = "Second", .lane = "Web"}, "lead", &second_);
        QVERIFY(!first_.isEmpty() && !second_.isEmpty());
    }

    void defaultsAreTaskAndThresholdAtTwelveTurns() {
        QCOMPARE(team().contextReset, QStringLiteral("task_and_threshold"));
        QCOMPARE(team().contextResetTurns, 12);
        const QJsonObject json = teams_->teamJson(team());
        QCOMPARE(json.value("context_reset").toString(), QStringLiteral("task_and_threshold"));
        QCOMPARE(json.value("context_reset_turns").toInt(), 12);
    }

    // A developer woken for another task starts fresh; the first sight of a context only
    // records it, and the fresh thread is adopted once the provider names it.
    void aDeveloperStartsFreshAtATaskBoundary() {
        QCOMPARE(reason("dev1", first_, "thread-a", 1), QString{});
        QCOMPARE(reason("dev1", first_, "thread-a", 3), QString{});
        const auto planned = teams_->planContextReset("dev1", second_, "thread-a", 4);
        QVERIFY(planned);
        QCOMPARE(planned->reason, QStringLiteral("task_changed"));
        QCOMPARE(planned->task, second_);
        QVERIFY(planned->briefing.contains(second_));
        // Not reset until the host says so: the same turn asks again and gets the same answer.
        QCOMPARE(reason("dev1", second_, "thread-a", 4), QStringLiteral("task_changed"));
        teams_->noteContextReset("dev1", second_, 4);
        QCOMPARE(reason("dev1", second_, "thread-b", 5), QString{});
        QCOMPARE(team().contexts.value("dev1").thread, QStringLiteral("thread-b"));
        QCOMPARE(team().contexts.value("dev1").task, second_);
        // The mark outlives the daemon.
        load();
        QCOMPARE(team().contexts.value("dev1").since, qint64{4});
    }

    void everyRoleStartsFreshPastTheThreshold() {
        QCOMPARE(reason("dev1", first_, "thread-a", 2), QString{});
        QCOMPARE(reason("dev1", first_, "thread-a", 13), QString{});
        QCOMPARE(reason("dev1", first_, "thread-a", 14), QStringLiteral("threshold"));
        // The leader works across tasks: only the threshold applies.
        QCOMPARE(reason("lead", first_, "lead-thread", 1), QString{});
        QCOMPARE(reason("lead", second_, "lead-thread", 2), QString{});
        QCOMPARE(reason("lead", second_, "lead-thread", 13), QStringLiteral("threshold"));
    }

    void aReviewerStartsFreshForAnotherReview() {
        QCOMPARE(reason("rev", first_, "rev-thread", 1), QString{});
        QCOMPARE(reason("rev", second_, "rev-thread", 2), QStringLiteral("task_changed"));
    }

    // A thread the team has not seen (a provider handoff) is the start of a new context.
    void aNewThreadIsAdoptedNotReset() {
        QCOMPARE(reason("dev1", first_, "thread-a", 1), QString{});
        QCOMPARE(reason("dev1", second_, "handoff-thread", 40), QString{});
        QCOMPARE(team().contexts.value("dev1").since, qint64{40});
        QCOMPARE(reason("dev1", second_, "", 41), QString{});
    }

    void neverAndThresholdOnly() {
        QVERIFY(teams_->update(team_, {{"context_reset", "threshold"}}, "user").ok());
        QCOMPARE(reason("dev1", first_, "thread-a", 1), QString{});
        QCOMPARE(reason("dev1", second_, "thread-a", 2), QString{});
        QCOMPARE(reason("dev1", second_, "thread-a", 13), QStringLiteral("threshold"));
        QVERIFY(teams_->update(team_, {{"context_reset", "never"}}, "user").ok());
        QCOMPARE(reason("dev1", first_, "thread-a", 100), QString{});
        QCOMPARE(reason("lead", first_, "lead-thread", 1), QString{});
        QCOMPARE(reason("lead", first_, "lead-thread", 100), QString{});
        // A threshold of 0 keeps only the task boundary.
        QVERIFY(teams_
                    ->update(team_,
                             {{"context_reset", "task_and_threshold"}, {"context_reset_turns", 0}},
                             "user")
                    .ok());
        QCOMPARE(reason("dev2", first_, "dev2-thread", 1), QString{});
        QCOMPARE(reason("dev2", first_, "dev2-thread", 500), QString{});
        QCOMPARE(reason("dev2", second_, "dev2-thread", 501), QStringLiteral("task_changed"));
    }

    void settingsAreValidatedAndKept() {
        for (const QJsonValue& bad : {QJsonValue(3), QJsonValue(101), QJsonValue(-1),
                                      QJsonValue(7.5), QJsonValue("12"), QJsonValue(true)}) {
            const auto refused = teams_->update(team_, {{"context_reset_turns", bad}}, "user");
            QVERIFY(!refused.ok());
            QCOMPARE(refused.error->code, QStringLiteral("invalid_payload"));
        }
        for (const QJsonValue& bad : {QJsonValue("sometimes"), QJsonValue(1), QJsonValue("")})
            QVERIFY(!teams_->update(team_, {{"context_reset", bad}}, "user").ok());
        // A refused change leaves the other one in the same command undone too.
        QVERIFY(
            !teams_->update(team_, {{"context_reset", "never"}, {"context_reset_turns", 2}}, "user")
                 .ok());
        QCOMPARE(team().contextReset, QStringLiteral("task_and_threshold"));
        QVERIFY(teams_
                    ->update(team_, {{"context_reset", "threshold"}, {"context_reset_turns", 4}},
                             "user")
                    .ok());
        QVERIFY(teams_->update(team_, {{"context_reset_turns", 100}}, "user").ok());
        load();
        QCOMPARE(team().contextReset, QStringLiteral("threshold"));
        QCOMPARE(team().contextResetTurns, 100);
        // A hand-edited document reads as the defaults.
        QJsonObject json = toJson(team());
        json.insert("context_reset", "sometimes");
        json.insert("context_reset_turns", 3);
        const Team read = teamFromJson(json);
        QCOMPARE(read.contextReset, QStringLiteral("task_and_threshold"));
        QCOMPARE(read.contextResetTurns, 12);
    }

    void theBriefingStaysWithinFourKilobytes() {
        Team big = team();
        big.goal = QString(5000, QLatin1Char('g'));
        Task* task = big.task(first_);
        task->owner = "dev1";
        task->state = state::inProgress;
        task->criteria.clear();
        for (int index = 0; index < maximumCriteria; ++index)
            task->criteria.append(
                {QStringLiteral("Criterion %1 ").arg(index) + QString(900, 'c'), index % 2 == 0});
        for (int index = 0; index < 30; ++index)
            task->history.append(
                {.at = nowIso(),
                 .actor = "dev1",
                 .kind = "note",
                 .text = QStringLiteral("entry-%1 ").arg(index) + QString(1500, QChar(0x1EC7))});
        const QString text = context_reset::briefing(big, *big.member("dev1"), first_);
        QVERIFY(text.toUtf8().size() <= context_reset::maximumBriefingBytes);
        QVERIFY(text.contains(first_));
        QVERIFY(text.contains("team_get"));
        // Small enough to be whole: the last five entries, and none before them.
        task->criteria = {{"Works", true}};
        for (HistoryEntry& entry : task->history)
            entry.text = entry.text.left(9);
        const QString whole = context_reset::briefing(big, *big.member("dev1"), first_);
        QVERIFY(whole.toUtf8().size() <= context_reset::maximumBriefingBytes);
        QVERIFY(whole.contains("- [x] Works"));
        QVERIFY(whole.contains("entry-25") && whole.contains("entry-29"));
        QVERIFY(!whole.contains("entry-24"));
        QVERIFY(whole.contains("lane \"Web\""));
    }

    void theLeaderBriefingListsAtMostTwentyOpenTasks() {
        Team big = team();
        for (int index = 0; index < 25; ++index)
            big.tasks.append({.id = QStringLiteral("X-%1").arg(index),
                              .title = QStringLiteral("Open %1").arg(index)});
        big.tasks.first().state = state::done;
        const QString text = context_reset::briefing(big, *big.member("lead"), {});
        QVERIFY(text.toUtf8().size() <= context_reset::maximumBriefingBytes);
        QVERIFY(text.contains("Open tasks (26):"));
        QCOMPARE(text.count(QStringLiteral("\n- ")), 21);
        QVERIFY(text.contains("… 6 more"));
        QVERIFY(!text.contains(QStringLiteral("%1 \"First\"").arg(first_)));
    }

    // Through the daemon: the leader's team turns start fresh past the threshold, with the
    // transcript note and the briefing; the user's turns never do.
    void theLeaderStartsFreshPastTheThresholdButNotForTheUser() {
        agent_test::Fixture fixture;
        QVERIFY(fixture.open());
        const auto command = [&](const QString& type, const QJsonObject& payload) {
            return fixture.host->handleCommand(agent_test::request(type, payload), "desktop-test");
        };
        const QJsonObject created =
            command("team.create", {{"repository", fixture.directory.path()},
                                    {"name", "Reset"},
                                    {"goal", "Ship the reset"},
                                    {"integration_branch", "integration"},
                                    {"plan", plan()}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(agent_test::text(created)));
        const QJsonValue teamId = created.value("team").toObject().value("id");
        const QJsonArray members = created.value("team").toObject().value("members").toArray();
        const QString leader = members.at(0).toObject().value("session_id").toString();
        const QString developer = members.at(1).toObject().value("session_id").toString();
        const QJsonObject changed =
            command("team.update", {{"team_id", teamId}, {"context_reset_turns", 4}});
        QVERIFY2(changed.value("ok").toBool(), qPrintable(agent_test::text(changed)));
        const QJsonObject read = fixture.agents(developer, "agents.team_get");
        QCOMPARE(read.value("team").toObject().value("context_reset_turns").toInt(), 4);
        QVERIFY(!read.value("team").toObject().contains("contexts"));

        const auto prompts = [&] {
            QList<QByteArray> turns;
            for (const auto& line : agent_test::logText(fixture.claudeLog).split('\n'))
                if (line.startsWith(("turn:" + leader).toUtf8()))
                    turns.append(line);
            return turns;
        };
        const auto ready = [&] {
            const auto session = storedSession(*fixture.journal, leader);
            return session && session->state == QStringLiteral("ready");
        };
        const auto wake = [&](const int index) {
            const int turns = fixture.count(leader, "turn.completed");
            const QJsonObject sent = fixture.agents(
                developer, "agents.send",
                {{"to", "leader"}, {"text", QStringLiteral("Update %1").arg(index)}});
            QVERIFY2(sent.value("ok").toBool(), qPrintable(agent_test::text(sent)));
            QTRY_VERIFY_WITH_TIMEOUT(fixture.count(leader, "turn.completed") > turns, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(ready(), 5000);
        };
        QTRY_VERIFY_WITH_TIMEOUT(fixture.count(leader, "turn.completed") >= 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(ready(), 5000);
        const QString firstThread = storedSession(*fixture.journal, leader)->threadId;
        QVERIFY(!firstThread.isEmpty());
        for (int index = 0; index < 8 && fixture.count(leader, "session.context_reset") == 0;
             ++index)
            wake(index);
        QCOMPARE(fixture.count(leader, "session.context_reset"), 1);
        const QJsonObject reset = fixture.payloads(leader, "session.context_reset").first();
        QCOMPARE(reset.value("reason").toString(), QStringLiteral("threshold"));
        QCOMPARE(reset.value("provider").toString(), QStringLiteral("claude"));
        QVERIFY2(prompts().last().contains("[KetPlus context reset]"),
                 prompts().last().constData());
        const QString transcript =
            QDir(fixture.directory.filePath("handoff/" + leader)).filePath("transcript.md");
        QVERIFY(QFileInfo::exists(transcript));
        // The old thread is kept for scoped deletion; the fresh one is another.
        const auto session = storedSession(*fixture.journal, leader);
        QVERIFY(session->providerThreads.contains(firstThread));
        QVERIFY(session->threadId != firstThread);
        // The briefing follows the note in the same prompt (the log splits it into lines).
        QVERIFY(agent_test::logText(fixture.claudeLog).contains("[KetPlus team briefing]"));
        QVERIFY(agent_test::logText(fixture.claudeLog).contains("Goal: Ship the reset"));
        const qsizetype resetTurns = prompts().size();

        // The user's turns, past the threshold, continue the same thread.
        for (int index = 0; index < 5; ++index) {
            QVERIFY(
                !fixture.start("claude", QStringLiteral("hello %1").arg(index), leader).isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(prompts().size() > resetTurns + index, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(ready(), 5000);
        }
        QCOMPARE(fixture.count(leader, "session.context_reset"), 1);
        for (qsizetype index = resetTurns; index < prompts().size(); ++index)
            QVERIFY(!prompts().at(index).contains("[KetPlus"));
        // The next team turn counts them, and starts fresh.
        wake(100);
        QCOMPARE(fixture.count(leader, "session.context_reset"), 2);
    }

    // A transcript that cannot be written keeps the thread: the turn runs on it.
    void aTranscriptFailureKeepsTheThread() {
        agent_test::Fixture fixture;
        QVERIFY(fixture.open());
        const auto command = [&](const QString& type, const QJsonObject& payload) {
            return fixture.host->handleCommand(agent_test::request(type, payload), "desktop-test");
        };
        const QJsonObject created =
            command("team.create", {{"repository", fixture.directory.path()},
                                    {"name", "Reset"},
                                    {"goal", "Ship the reset"},
                                    {"integration_branch", "integration"},
                                    {"plan", plan()}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(agent_test::text(created)));
        const QJsonValue teamId = created.value("team").toObject().value("id");
        const QJsonArray members = created.value("team").toObject().value("members").toArray();
        const QString leader = members.at(0).toObject().value("session_id").toString();
        const QString developer = members.at(1).toObject().value("session_id").toString();
        QVERIFY(command("team.update", {{"team_id", teamId}, {"context_reset_turns", 4}})
                    .value("ok")
                    .toBool());
        // A file where the transcript's directory would be.
        QVERIFY(QDir().mkpath(fixture.directory.filePath("handoff")));
        QFile blocker(fixture.directory.filePath("handoff/" + leader));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        const auto ready = [&] {
            const auto session = storedSession(*fixture.journal, leader);
            return session && session->state == QStringLiteral("ready");
        };
        QTRY_VERIFY_WITH_TIMEOUT(fixture.count(leader, "turn.completed") >= 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(ready(), 5000);
        const QString thread = storedSession(*fixture.journal, leader)->threadId;
        QVERIFY(!thread.isEmpty());
        for (int index = 0; index < 7; ++index) {
            const int turns = fixture.count(leader, "turn.completed");
            QVERIFY(
                fixture
                    .agents(developer, "agents.send",
                            {{"to", "leader"}, {"text", QStringLiteral("Update %1").arg(index)}})
                    .value("ok")
                    .toBool());
            QTRY_VERIFY_WITH_TIMEOUT(fixture.count(leader, "turn.completed") > turns, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(ready(), 5000);
        }
        QCOMPARE(fixture.count(leader, "session.context_reset"), 0);
        QCOMPARE(storedSession(*fixture.journal, leader)->threadId, thread);
    }
};

QTEST_GUILESS_MAIN(TeamContextResetTest)
#include "TeamContextResetTest.moc"
