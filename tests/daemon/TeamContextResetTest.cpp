// T-1: when a turn the team starts gets a fresh native context, the team's context reset
// settings, and the briefing the fresh context begins with. The coordinator cases use a fake
// runtime; the protocol cases the scripted fake providers. No real provider is started.
#include "teams/TeamContextReset.h"
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "storage/LocalJournal.h"

#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>
#include <QTimeZone>

using namespace ketplus::daemon;
using namespace team_test;

namespace {

std::optional<StoredSession> storedSession(const LocalJournal& journal, const QString& id) {
    for (const StoredSession& session : journal.sessions())
        if (session.id == id)
            return session;
    return std::nullopt;
}

void git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", arguments);
    QVERIFY(process.waitForFinished(15000));
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
}

QJsonObject plan() {
    return {{"leader", QJsonObject{{"provider", "claude"}}},
            {"developers", QJsonObject{{"count", 1}, {"provider", "codex"}}},
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
    // The thread's activity `idleMinutes` after its last turn ended, with `tokens` of context.
    static context_reset::ThreadActivity activity(const qint64 idleMinutes, const qint64 tokens) {
        const QDateTime now(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
        return {
            .lastTurnEnded = now.addSecs(-idleMinutes * 60), .contextTokens = tokens, .now = now};
    }
    QString coldReason(const QString& session, const QString& hint, const QString& thread,
                       const qint64 turn, const context_reset::ThreadActivity& when) {
        const auto planned = teams_->planContextReset(session, hint, thread, turn, when);
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
        QCOMPARE(team().contextResetIdleMinutes, 60);
        QCOMPARE(json.value("context_reset_idle_minutes").toInt(), 60);
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

    void sizeThresholdUsesTheLastReportedRequest() {
        QCOMPARE(coldReason("lead", first_, "size-thread", 1, activity(0, 150000)), QString{});
        QCOMPARE(coldReason("lead", first_, "size-thread", 2, activity(0, 149999)), QString{});
        QCOMPARE(coldReason("lead", first_, "size-thread", 2, activity(0, 150000)),
                 QStringLiteral("context_size"));
        QVERIFY(!teams_->update(team_, {{"context_reset_tokens", 5.5}}, "user").ok());
        QVERIFY(teams_->update(team_, {{"context_reset_tokens", 200000}}, "user").ok());
        QCOMPARE(coldReason("lead", first_, "size-thread", 2, activity(0, 150000)), QString{});
        QCOMPARE(coldReason("lead", first_, "size-thread", 2, activity(0, 200000)),
                 QStringLiteral("context_size"));
        load();
        QCOMPARE(team().contextResetTokens, 200000);
        QVERIFY(teams_->update(team_, {{"context_reset_tokens", 0}}, "user").ok());
        QCOMPARE(coldReason("lead", first_, "size-thread", 2, activity(0, 200000)), QString{});
    }

    // T-10: a member woken after its thread rested an hour, with a large context, starts fresh
    // for any role. The first sight of a thread only records it.
    void aColdCacheStartsEveryRoleFresh() {
        const auto cold = activity(61, 60000);
        for (const QString& session : QStringList{"lead", "dev1", "rev"}) {
            const QString thread = session + "-thread";
            QCOMPARE(coldReason(session, first_, thread, 1, cold), QString{});
            const auto planned = teams_->planContextReset(session, first_, thread, 2, cold);
            QVERIFY2(planned, qPrintable(session));
            QCOMPARE(planned->reason, QStringLiteral("cache_cold"));
            QVERIFY(!planned->briefing.isEmpty());
        }
    }

    // Both must hold: an idle at least the team's setting, and a known context of 50,000
    // tokens or more.
    void aColdCacheNeedsTheIdleAndTheSize() {
        QCOMPARE(coldReason("lead", first_, "t", 1, activity(90, 90000)), QString{});
        const auto reasonFor = [&](const qint64 minutes, const qint64 tokens) {
            return coldReason("lead", first_, "t", 2, activity(minutes, tokens));
        };
        QCOMPARE(reasonFor(60, 50000), QStringLiteral("cache_cold"));
        QCOMPARE(reasonFor(59, 90000), QString{});
        QCOMPARE(reasonFor(90, 49999), QString{});
        QCOMPARE(reasonFor(90, 0), QString{});
        // A size the provider never reported (or none yet on this thread).
        QCOMPARE(reasonFor(90, -1), QString{});
        auto unknownEnd = activity(90, 90000);
        unknownEnd.lastTurnEnded = {};
        QCOMPARE(coldReason("lead", first_, "t", 2, unknownEnd), QString{});
        QCOMPARE(coldReason("lead", first_, "t", 2, {}), QString{});
        // The team's own setting is the idle.
        QVERIFY(teams_->update(team_, {{"context_reset_idle_minutes", 5}}, "user").ok());
        QCOMPARE(reasonFor(5, 60000), QStringLiteral("cache_cold"));
        QCOMPARE(reasonFor(4, 60000), QString{});
        QVERIFY(teams_->update(team_, {{"context_reset_idle_minutes", 1440}}, "user").ok());
        QCOMPARE(reasonFor(1439, 60000), QString{});
        QCOMPARE(reasonFor(1440, 60000), QStringLiteral("cache_cold"));
    }

    // Off with a zero idle and with "never"; it also works under "threshold".
    void aColdCacheIsOffForNeverAndAZeroIdle() {
        const auto cold = activity(300, 90000);
        QCOMPARE(coldReason("dev1", first_, "t", 1, cold), QString{});
        QCOMPARE(coldReason("dev1", first_, "t", 2, cold), QStringLiteral("cache_cold"));
        QVERIFY(teams_->update(team_, {{"context_reset_idle_minutes", 0}}, "user").ok());
        QCOMPARE(coldReason("dev1", first_, "t", 2, cold), QString{});
        QVERIFY(teams_->update(team_, {{"context_reset_idle_minutes", 60}}, "user").ok());
        QVERIFY(teams_->update(team_, {{"context_reset", "never"}}, "user").ok());
        QCOMPARE(coldReason("dev1", first_, "t", 2, cold), QString{});
        QCOMPARE(coldReason("lead", first_, "t2", 1, cold), QString{});
        QCOMPARE(coldReason("lead", first_, "t2", 2, cold), QString{});
        QVERIFY(teams_->update(team_, {{"context_reset", "threshold"}}, "user").ok());
        QCOMPARE(coldReason("dev1", first_, "t", 2, cold), QStringLiteral("cache_cold"));
    }

    // The older reasons keep their order, and their behaviour without activity is unchanged.
    void aColdCacheYieldsToTheOlderReasons() {
        const auto cold = activity(300, 90000);
        QCOMPARE(coldReason("dev1", first_, "t", 1, cold), QString{});
        QCOMPARE(coldReason("dev1", second_, "t", 2, cold), QStringLiteral("task_changed"));
        QCOMPARE(coldReason("dev1", first_, "t", 13, cold), QStringLiteral("threshold"));
        QCOMPARE(coldReason("dev1", first_, "t", 13, {}), QStringLiteral("threshold"));
    }

    void theIdleSettingIsValidatedAndKept() {
        for (const QJsonValue& bad : {QJsonValue(4), QJsonValue(1441), QJsonValue(-1),
                                      QJsonValue(7.5), QJsonValue("60"), QJsonValue(true)}) {
            const auto refused =
                teams_->update(team_, {{"context_reset_idle_minutes", bad}}, "user");
            QVERIFY(!refused.ok());
            QCOMPARE(refused.error->code, QStringLiteral("invalid_payload"));
        }
        QCOMPARE(team().contextResetIdleMinutes, 60);
        for (const int good : {5, 1440, 0, 90}) {
            QVERIFY(teams_->update(team_, {{"context_reset_idle_minutes", good}}, "user").ok());
            QCOMPARE(team().contextResetIdleMinutes, good);
        }
        // One refused value undoes the others in the same command.
        QVERIFY(!teams_
                     ->update(team_,
                              {{"context_reset_turns", 5}, {"context_reset_idle_minutes", 2}},
                              "user")
                     .ok());
        QCOMPARE(team().contextResetTurns, 12);
        load();
        QCOMPARE(team().contextResetIdleMinutes, 90);
        QCOMPARE(teams_->teamJson(team()).value("context_reset_idle_minutes").toInt(), 90);
        // A document from before the setting, and a hand-edited one, read as the default.
        QJsonObject json = toJson(team());
        json.remove("context_reset_idle_minutes");
        QCOMPARE(teamFromJson(json).contextResetIdleMinutes, 60);
        json.insert("context_reset_idle_minutes", 3);
        QCOMPARE(teamFromJson(json).contextResetIdleMinutes, 60);
        json.insert("context_reset_idle_minutes", 0);
        QCOMPARE(teamFromJson(json).contextResetIdleMinutes, 0);
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
                {QStringLiteral("Criterion %1 ").arg(index) + QString(450, QChar(0x1EC7)),
                 index % 2 == 0});
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
        QVERIFY(text.contains("briefing shortened"));
        QVERIFY(text.endsWith(QStringLiteral("\n\n")));
        QCOMPARE(QString::fromUtf8(text.toUtf8()), text);
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

    void theBriefingListsTaskFilesAndSummarizesCriteria() {
        Team project = team();
        Task* task = project.task(first_);
        task->description = QStringLiteral("Read `docs/design.md` and `src/main.cpp`. "
                                           "Skip `/tmp/secret.txt`, `../other.md`, `0.1.53`, "
                                           "`team.name`, and `node.js`.");
        task->criteria = {{QStringLiteral("Verify `src/main.cpp` with `tests/reset.cpp`."), true},
                          {QStringLiteral("Document the limit. ") + QString(600, 'a'), false}};
        const QString brief = context_reset::briefing(project, *project.member("dev1"), first_);
        QVERIFY(brief.contains("Files to read in the task worktree:\n- AGENTS.md\n"
                               "- docs/design.md\n- src/main.cpp\n- tests/reset.cpp\n"));
        QVERIFY(!brief.contains("\n- /tmp/secret.txt"));
        QVERIFY(!brief.contains("\n- ../other.md"));
        QVERIFY(!brief.contains("\n- 0.1.53"));
        QVERIFY(!brief.contains("\n- team.name"));
        QVERIFY(!brief.contains("\n- node.js"));
        QVERIFY(brief.contains("Acceptance criteria (summary):"));
        QVERIFY(brief.contains("- [x] Verify"));
        QVERIFY(brief.contains("- [ ] Document the limit."));
        QVERIFY(brief.contains("more characters)"));
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

    // T-10, through the daemon: a leader woken after an hour of rest, with the 85,000 tokens
    // the fake provider reports, starts fresh with reason cache_cold; the user's turns, a wake
    // of an hour of rest too short, a zero idle and "never" do not. The rest is timed from the
    // end of the last turn, even a long one.
    void aColdCacheStartsAWokenLeaderFresh() {
        agent_test::Fixture fixture;
        QVERIFY(fixture.open());
        const QDateTime base(QDate(2026, 1, 1), QTime(9, 0), QTimeZone::UTC);
        qint64 offsetMinutes = 0;
        fixture.host->setClockForTest([&] { return base.addSecs(offsetMinutes * 60); });
        const auto command = [&](const QString& type, const QJsonObject& payload) {
            return fixture.host->handleCommand(agent_test::request(type, payload), "desktop-test");
        };
        const QJsonObject created =
            command("team.create", {{"repository", fixture.directory.path()},
                                    {"name", "Cold"},
                                    {"goal", "Ship it"},
                                    {"integration_branch", "integration"},
                                    {"plan", plan()}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(agent_test::text(created)));
        const QJsonValue teamId = created.value("team").toObject().value("id");
        const QJsonArray members = created.value("team").toObject().value("members").toArray();
        const QString leader = members.at(0).toObject().value("session_id").toString();
        const QString developer = members.at(1).toObject().value("session_id").toString();
        const auto ready = [&] {
            const auto session = storedSession(*fixture.journal, leader);
            return session && session->state == QStringLiteral("ready");
        };
        // The minutes the leader's next turn adds to the clock before it ends (a long turn).
        qint64 turnMinutes = 0;
        QObject::connect(
            fixture.host.get(), &SessionHost::eventReady, [&](const QJsonObject& event) {
                if (event.value("event_type").toString() == QStringLiteral("usage.context") &&
                    event.value("session_id").toString() == leader)
                    offsetMinutes += std::exchange(turnMinutes, 0);
            });
        const auto wake = [&](const QString& text) {
            const int turns = fixture.count(leader, "turn.completed");
            const QJsonObject sent =
                fixture.agents(developer, "agents.send", {{"to", "leader"}, {"text", text}});
            QVERIFY2(sent.value("ok").toBool(), qPrintable(agent_test::text(sent)));
            QTRY_VERIFY_WITH_TIMEOUT(fixture.count(leader, "turn.completed") > turns, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(ready(), 5000);
        };
        const auto resets = [&] { return fixture.count(leader, "session.context_reset"); };
        QTRY_VERIFY_WITH_TIMEOUT(fixture.count(leader, "turn.completed") >= 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(ready(), 5000);
        const QString firstThread = storedSession(*fixture.journal, leader)->threadId;
        QVERIFY(!firstThread.isEmpty());

        // Rested 59 minutes: the cache is warm.
        offsetMinutes = 59;
        wake("Early");
        QCOMPARE(resets(), 0);
        // The user's turn after three hours is never reset.
        offsetMinutes += 180;
        QVERIFY(!fixture.start("claude", "hello", leader).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.count(leader, "turn.completed") >= 3, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(ready(), 5000);
        QCOMPARE(resets(), 0);
        // A long turn: it takes two hours, and ends five minutes before the next wake. The
        // rest is five minutes, not the two hours since the message.
        turnMinutes = 120;
        wake("Long");
        offsetMinutes += 5;
        wake("Soon after");
        QCOMPARE(resets(), 0);

        // A zero idle and "never" keep the thread however long it rested.
        QVERIFY(command("team.update", {{"team_id", teamId}, {"context_reset_idle_minutes", 0}})
                    .value("ok")
                    .toBool());
        offsetMinutes += 600;
        wake("Zero");
        QCOMPARE(resets(), 0);
        QVERIFY(command("team.update", {{"team_id", teamId},
                                        {"context_reset_idle_minutes", 60},
                                        {"context_reset", "never"}})
                    .value("ok")
                    .toBool());
        offsetMinutes += 600;
        wake("Never");
        QCOMPARE(resets(), 0);

        // Back on: an hour of rest and a large context start a fresh thread, with the briefing.
        QVERIFY(
            command("team.update", {{"team_id", teamId}, {"context_reset", "task_and_threshold"}})
                .value("ok")
                .toBool());
        offsetMinutes += 61;
        wake("Late");
        QCOMPARE(resets(), 1);
        const QJsonObject reset = fixture.payloads(leader, "session.context_reset").first();
        QCOMPARE(reset.value("reason").toString(), QStringLiteral("cache_cold"));
        const auto session = storedSession(*fixture.journal, leader);
        QVERIFY(session->providerThreads.contains(firstThread));
        QVERIFY(session->threadId != firstThread);
        QVERIFY(QFileInfo::exists(
            QDir(fixture.directory.filePath("handoff/" + leader)).filePath("transcript.md")));
        QVERIFY(agent_test::logText(fixture.claudeLog).contains("[KetPlus team briefing]"));
        // The fresh thread has not rested: the next wake continues it.
        wake("Right after");
        QCOMPARE(resets(), 1);
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

    // T-1 review f1: the task a team message names belongs to that message, and the turn that
    // delivers it takes it, even one that does not reset. Here T-2's provider switch delivers
    // T-2's assignment in a handoff, while the developer still works on T-1. Its task, left
    // over, would mark the new thread with T-2, and the next wake about no task would start
    // the developer fresh for a task boundary that never was.
    void aMessageDeliveredInAHandoffLeavesNoTaskForTheNextWake() {
        agent_test::Fixture fixture;
        QVERIFY(fixture.open());
        const auto command = [&](const QString& type, const QJsonObject& payload) {
            return fixture.host->handleCommand(agent_test::request(type, payload), "desktop-test");
        };
        const QString repository = fixture.directory.filePath("shop");
        QVERIFY(QDir().mkpath(repository));
        git(repository, {"init", "-q", "-b", "integration"});
        git(repository, {"-c", "user.email=t@example.com", "-c", "user.name=T", "commit", "-q",
                         "--allow-empty", "-m", "start"});
        const QJsonObject created = command("team.create", {{"repository", repository},
                                                            {"name", "Reset"},
                                                            {"goal", "Ship the reset"},
                                                            {"integration_branch", "integration"},
                                                            {"plan", plan()}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(agent_test::text(created)));
        const QJsonValue teamId = created.value("team").toObject().value("id");
        const QJsonArray members = created.value("team").toObject().value("members").toArray();
        const QString leader = members.at(0).toObject().value("session_id").toString();
        const QString developer = members.at(1).toObject().value("session_id").toString();
        const auto ready = [&](const QString& id) {
            const auto session = storedSession(*fixture.journal, id);
            return session && session->state == QStringLiteral("ready");
        };
        const auto settle = [&] {
            QTRY_VERIFY_WITH_TIMEOUT(ready(leader) && ready(developer), 8000);
        };
        const auto finished = [&](const int before) {
            QTRY_VERIFY_WITH_TIMEOUT(fixture.count(developer, "turn.completed") > before, 8000);
        };
        const auto assign = [&](const QString& title) {
            const QJsonObject task =
                fixture.agents(leader, "agents.team_task_create",
                               {{"title", title}, {"criteria", QJsonArray{"Works"}}});
            QVERIFY2(task.value("ok").toBool(), qPrintable(agent_test::text(task)));
            const QString id = task.value("task").toObject().value("id").toString();
            const QJsonObject assigned =
                fixture.agents(leader, "agents.team_task_assign",
                               {{"task_id", id}, {"owner", "#" + fixture.number(developer)}});
            QVERIFY2(assigned.value("ok").toBool(), qPrintable(agent_test::text(assigned)));
        };
        const auto wake = [&](const QString& text) {
            const int turns = fixture.count(developer, "turn.completed");
            const QJsonObject sent = fixture.agents(
                leader, "agents.send", {{"to", fixture.number(developer)}, {"text", text}});
            QVERIFY2(sent.value("ok").toBool(), qPrintable(agent_test::text(sent)));
            finished(turns);
            settle();
        };
        QTRY_VERIFY_WITH_TIMEOUT(fixture.count(leader, "turn.completed") >= 1, 8000);
        settle();

        // T-1 is the developer's task: the approved plan sends its assignment, and it starts.
        int turns = fixture.count(developer, "turn.completed");
        assign("First");
        QString decision;
        for (const QJsonValue& value : command("team.get", {{"team_id", teamId}})
                                           .value("team")
                                           .toObject()
                                           .value("decisions")
                                           .toArray())
            if (value.toObject().value("kind").toString() == QStringLiteral("first_plan"))
                decision = value.toObject().value("id").toString();
        QVERIFY(command("team.decision.answer",
                        {{"team_id", teamId}, {"decision_id", decision}, {"choice", "Start"}})
                    .value("ok")
                    .toBool());
        finished(turns);
        settle();
        const QJsonObject started = fixture.agents(developer, "agents.team_task_update",
                                                   {{"task_id", "T-1"}, {"state", "in_progress"}});
        QVERIFY2(started.value("ok").toBool(), qPrintable(agent_test::text(started)));
        settle();
        wake("Keep going");
        QCOMPARE(fixture.count(developer, "session.context_reset"), 0);

        // The developer moves to Claude; T-2's assignment is the turn that takes it there.
        const QJsonObject switched = command(
            "team.provider.set",
            {{"team_id", teamId}, {"provider", "claude"}, {"members", QJsonArray{developer}}});
        QVERIFY2(switched.value("ok").toBool(), qPrintable(agent_test::text(switched)));
        turns = fixture.count(developer, "turn.completed");
        assign("Second");
        finished(turns);
        settle();
        QCOMPARE(storedSession(*fixture.journal, developer)->adapter, QStringLiteral("claude"));

        // The wakes after it are about no task: the developer keeps its new context.
        wake("Status?");
        wake("Status again?");
        QCOMPARE(fixture.count(developer, "session.context_reset"), 0);
    }
};

QTEST_GUILESS_MAIN(TeamContextResetTest)
#include "TeamContextResetTest.moc"
