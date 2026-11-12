// ADR 0114 §7 and open point 2: what starts a turn, the leader's digest, pause and the budget
// of automatic turns. The coordinator runs on a real journal with a fake runtime and a short
// quiet period. No provider is started.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "storage/LocalJournal.h"
#include "teams/TeamRemoteView.h"
#include "teams/TeamVerdictLine.h"

#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace ketplus::daemon;
using namespace ketplus::daemon::teams;
using namespace team_test;

class TeamFlowTest final : public QObject {
    Q_OBJECT

  private:
    static constexpr int quiet = 60;
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
        teams_->setQuietPeriod(quiet);
        teams_->load();
    }

    void start(const int budget = defaultTurnBudget) {
        Team draft = standardTeam();
        draft.turnBudget = budget;
        team_ = teams_->create(draft, "user").teamId;
        QVERIFY(team_ > 0);
    }

    QString task(const QString& title) {
        QString id;
        (void)teams_->createTask(team_, {.title = title, .lane = "Web", .criteria = {"Works"}},
                                 "lead", &id);
        return id;
    }

    const Team& team() { return *teams_->team(team_); }

    // Waits longer than the quiet period, letting its timer fire.
    static void settle() { QTest::qWait(quiet * 4); }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        QFile::remove(directory_.filePath("journal.sqlite3"));
        host_ = {};
        for (const QString& id : {"lead", "dev1", "dev2"})
            host_.add(id);
        host_.add("rev", "claude");
        reopen();
    }

    void anAssignmentWakesOnlyItsNewOwner() {
        start();
        const QString id = task("Cart");
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QCOMPARE(host_.messages.size(), 1);
        QCOMPARE(host_.messages.first().kind, QStringLiteral("assignment"));
        QCOMPARE(host_.messages.first().toSession, QStringLiteral("dev1"));
        QVERIFY(teams_->handoverTask(team_, id, "dev2", "Api knows the cart", "lead").ok());
        QCOMPARE(host_.messages.size(), 2);
        QCOMPARE(host_.messages.last().toSession, QStringLiteral("dev2"));
        QCOMPARE(team().automaticTurns, 2);
    }

    void aNoteOrAMetMarkAloneStartsNoTurn() {
        start();
        const QString id = task("Cart");
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        host_.messages.clear();
        QVERIFY(teams_->updateTask(team_, id, {{"note", "Halfway"}}, "dev1").ok());
        QVERIFY(teams_
                    ->updateTask(team_, id,
                                 {{"met", QJsonArray{QJsonObject{{"index", 0}, {"met", true}}}}},
                                 "dev1")
                    .ok());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        settle();
        teams_->memberTurnEnded("lead");
        teams_->memberTurnEnded("dev1");
        QVERIFY(host_.messages.isEmpty());
    }

    void tenUpdatesDuringALeaderTurnBecomeOneDigestAfterIt() {
        start();
        const QString id = task("Cart");
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        host_.messages.clear();
        host_.setIdle("lead", false);
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        for (int index = 0; index < 5; ++index) {
            QVERIFY(teams_
                        ->moveTask(team_, id, state::blocked, "dev1",
                                   QStringLiteral("Question %1?").arg(index))
                        .ok());
            QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        }
        QVERIFY(teams_->updateTask(team_, id, {{"note", "A note rides along"}}, "dev1").ok());
        settle();
        QVERIFY2(host_.messages.isEmpty(), "No turn starts while the leader's turn runs.");
        host_.setIdle("lead", true);
        teams_->memberTurnEnded("lead");
        QCOMPARE(host_.messages.size(), 1);
        const TeamMessage digest = host_.messages.first();
        QCOMPARE(digest.kind, QStringLiteral("digest"));
        QCOMPARE(digest.toSession, QStringLiteral("lead"));
        QVERIFY(digest.fromSession.isEmpty());
        QVERIFY(digest.body.contains("T-1 \"Cart\""));
        QCOMPARE(digest.body.count("[needs you]"), 5 + 1); // five questions, plus the footer
        QVERIFY(digest.body.contains("A note rides along"));
        // Nothing waits any more: neither the next turn end nor the timer sends another.
        teams_->memberTurnEnded("lead");
        settle();
        QCOMPARE(host_.messages.size(), 1);
    }

    // T-75: a task of dev1 with a pull request, in review, and the leader's inbox empty.
    QString reviewed(const QString& title) {
        const QString id = task(title);
        if (!teams_->assignTask(team_, id, "dev1", "lead").ok() ||
            !teams_
                 ->updateTask(team_, id, {{"pull_request", "https://example.test/pull/12"}}, "lead")
                 .ok() ||
            !teams_->moveTask(team_, id, state::inProgress, "dev1").ok() ||
            !teams_->moveTask(team_, id, state::inReview, "dev1").ok())
            return {};
        settle();
        host_.messages.clear();
        return id;
    }
    QString verdictUpdate(const QString& id) {
        for (const auto& update : journal_->teamUpdates(team_, false, 100))
            if ((update.kind == "verdict" || update.kind == "round_limit") && update.taskId == id)
                return update.actionable ? update.text : QStringLiteral("not actionable");
        return {};
    }

    // T-75: a findings verdict sends the task back to its developer, and the leader reads it
    // in its next digest, with the round, the reviewer and the findings by severity.
    void aFindingsVerdictMakesTheLeadersDigestDue() {
        start();
        const QString id = reviewed("Cart");
        QVERIFY(!id.isEmpty());
        QVERIFY(host_.of("digest").isEmpty());
        finishReview(*teams_, "rev",
                     "## Findings\n- **L1 — Low — `web/cart.ts:4`:** no total.\n"
                     "- **L2 — Low — `web/cart.ts:9`:** rounding.\n"
                     "- **N1 — Nit — `web/cart.ts:2`:** a name.\nL1 again: see above.\n"
                     "Verdict: findings",
                     false);
        QCOMPARE(team().task(id)->state, state::inProgress);
        const QString line = verdictUpdate(id);
        QCOMPARE(line,
                 QStringLiteral("Verdict on %1 (PR https://example.test/pull/12), round 1 of 2 by "
                                "Reviewer: findings (0 High, 0 Medium, 2 Low, 1 Nit). Back to its "
                                "developer; decide on any finding outside the task's scope.")
                     .arg(id));
        QTRY_COMPARE_WITH_TIMEOUT(host_.of("digest").size(), 1, 2000);
        const TeamMessage digest = host_.of("digest").first();
        QCOMPARE(digest.toSession, QStringLiteral("lead"));
        QVERIFY(digest.body.contains(QStringLiteral("- [needs you] ") + line));
        // The developer gets the findings; nothing goes from member to member besides.
        QCOMPARE(host_.of("review").size(), 1);
        QCOMPARE(host_.of("review").first().toSession, QStringLiteral("dev1"));
    }

    void aPassVerdictMakesTheLeadersDigestDue() {
        start();
        const QString id = reviewed("Cart");
        finishReview(*teams_, "rev", "Nothing to fix.\nVerdict: no findings", true);
        QCOMPARE(team().task(id)->state, state::ready);
        const QString line = verdictUpdate(id);
        QCOMPARE(line, QStringLiteral("Verdict on %1 (PR https://example.test/pull/12), round 1 "
                                      "of 2 by Reviewer: no findings. Ready to merge.")
                           .arg(id));
        QTRY_COMPARE_WITH_TIMEOUT(host_.of("digest").size(), 1, 2000);
        QVERIFY(host_.of("digest").first().body.contains(line));
    }

    void aRoundLimitVerdictMakesTheLeadersDigestDue() {
        start();
        QVERIFY(teams_->update(team_, {{"max_rounds", 1}}, "user").ok());
        const QString id = reviewed("Cart");
        host_.messages.clear();
        finishReview(*teams_, "rev", "web/cart.ts:4 high: no total.\nVerdict: findings", false);
        QCOMPARE(team().task(id)->state, state::blocked);
        const QString line = verdictUpdate(id);
        QVERIFY2(line.endsWith("round 1 of 1 by Reviewer: findings (1 High, 0 Medium, 0 Low, "
                               "0 Nit). The round limit is reached: you decide."),
                 qPrintable(line));
        QTRY_COMPARE_WITH_TIMEOUT(host_.of("digest").size(), 1, 2000);
        QVERIFY(host_.of("digest").first().body.contains(line));
        QVERIFY(host_.of("review").isEmpty());
        // One line says the limit is reached: the verdict is the round_limit warning.
        int limits = 0;
        for (const auto& update : journal_->teamUpdates(team_, false, 100))
            limits += update.kind == "round_limit" || update.kind == "verdict" ? 1 : 0;
        QCOMPARE(limits, 1);
    }

    // The team page's Updates and the phone's team payload carry the same verdict line.
    void theVerdictLineIsInTheTeamPageAndThePhonePayload() {
        start();
        const QString id = reviewed("Cart");
        finishReview(*teams_, "rev", "- **L1, Low.** `web/cart.ts:4`: no total.\nVerdict: findings",
                     false);
        const QString line = verdictUpdate(id);
        QVERIFY(line.contains("(0 High, 0 Medium, 1 Low, 0 Nit)"));
        const QJsonArray page = teams_->updatesJson(team_);
        const auto has = [&line](const QJsonArray& updates, bool* warning = nullptr) {
            for (const QJsonValue& value : updates) {
                const QJsonObject update = value.toObject();
                if (update.value("text").toString() != line)
                    continue;
                if (warning)
                    *warning = update.value("warning").toBool();
                return update.value("actionable").toBool();
            }
            return false;
        };
        QVERIFY(has(page));
        bool warning = true;
        QVERIFY(has(remote::updates(page, teams_->teamJson(team())), &warning));
        QVERIFY(!warning);
    }

    // A busy leader reads the verdict when its turn ends, not before.
    void aBusyLeaderReadsTheVerdictWhenItsTurnEnds() {
        start();
        const QString id = reviewed("Cart");
        host_.setIdle("lead", false);
        finishReview(*teams_, "rev", "Verdict: findings", false);
        settle();
        QVERIFY(host_.of("digest").isEmpty());
        host_.setIdle("lead", true);
        teams_->memberTurnEnded("lead");
        QTRY_COMPARE_WITH_TIMEOUT(host_.of("digest").size(), 1, 2000);
        QVERIFY(host_.of("digest").first().body.contains(QStringLiteral("Verdict on %1").arg(id)));
        QVERIFY(host_.of("digest").first().body.contains("severity not stated"));
    }

    void severitiesAreCountedOncePerFinding() {
        using verdict_line::count;
        using verdict_line::counts;
        QCOMPARE(counts(count("**M1 — Medium — a.cpp:1:** x\n**M1** again\n- L3 - low: y\n"
                              "F2: Critical z\nVerdict: findings")),
                 QStringLiteral("1 High, 1 Medium, 1 Low, 0 Nit"));
        QCOMPARE(counts(count("a.cpp:4 medium: x\nb.cpp:2 nit: y\nVerdict: findings")),
                 QStringLiteral("0 High, 1 Medium, 0 Low, 1 Nit"));
        QCOMPARE(counts(count("Some issues.\nVerdict: findings")),
                 QStringLiteral("severity not stated"));
    }

    // The formats the team's reviewers write, as they reached the board.
    void severitiesAreCountedInTheReviewersFormats() {
        using verdict_line::count;
        using verdict_line::counts;
        // "- **H1, High.** …", with the ids mentioned again in the prose and a summary.
        QCOMPARE(counts(count("**Findings**\n- **H1, High.** `a.cpp:33`: both patterns miss.\n"
                              "  - Counts — High: 0, Medium: 1, Low: 2 reads as one High.\n"
                              "- **M1, Medium.** `a.cpp:38`: fixed findings count.\n"
                              "- **L1, Low.** `b.cpp:134`: no test.\n"
                              "- **N1, Nit.** `c.cpp:440`: three lines.\n"
                              "- **N2, Nit.** `sources.cmake:135`: order.\n"
                              "- **N3, Nit.** `a.h:22`: not the format. Fix with H1.\n"
                              "In short: 1 High, 1 Medium, 1 Low and 3 Nit.\nVerdict: findings")),
                 QStringLiteral("1 High, 1 Medium, 1 Low, 3 Nit"));
        // "- **L1 (Low), file:**".
        QCOMPARE(counts(count("- **L1 (Low), `a.cpp:12`:** stale summary.\n"
                              "- **N1 (Nit), `b.qml:3`:** spacing.\nVerdict: findings")),
                 QStringLiteral("0 High, 0 Medium, 1 Low, 1 Nit"));
        // "- **Low**, `file`:", without ids.
        QCOMPARE(counts(count("- **Low**, `a.cpp:4`: no total.\n- **Medium**, `b.cpp:9`: race.\n"
                              "- Low-level calls are fine.\nVerdict: findings")),
                 QStringLiteral("0 High, 1 Medium, 1 Low, 0 Nit"));
    }

    // Round 2 lists round 1's findings again with what became of them; only open ones count.
    void settledFindingsAreNotCounted() {
        using verdict_line::count;
        using verdict_line::counts;
        QCOMPARE(counts(count("- L1 — Low — fixed in abc123.\n- **N1 (Nit)** ✅ fixed\n"
                              "✅ **N2, Nit.** sorted.\n- N3 — Nit — still open: the doc.\n"
                              "Verdict: findings")),
                 QStringLiteral("0 High, 0 Medium, 0 Low, 1 Nit"));
        QCOMPARE(counts(count("- **L1, Low.** đã sửa.\nVerdict: findings")),
                 QStringLiteral("0 High, 0 Medium, 0 Low, 0 Nit"));
        QCOMPARE(counts(count("Counts — High: 0, Medium: 1, Low: 2\nVerdict: findings")),
                 QStringLiteral("severity not stated"));
        // T-62 r2 as it reached the board: the mark comes after the finding's text.
        const QString settledLine =
            "- **L1 (Low), xoá sạch thì trang mất tóm tắt:** ✅ **đã sửa.**\n";
        QCOMPARE(counts(count(settledLine + settledLine + settledLine + "Verdict: findings")),
                 QStringLiteral("0 High, 0 Medium, 0 Low, 0 Nit"));
        QCOMPARE(counts(count("- **L1 (Low), xoá sạch thì trang mất tóm tắt:** ✅ **đã sửa.**\n"
                              "- **L2 (Low), danh sách vẫn giữ dòng cũ:** ❌ chưa sửa.\n"
                              "Verdict: findings")),
                 QStringLiteral("0 High, 0 Medium, 1 Low, 0 Nit"));
        QCOMPARE(counts(count("- **N1 (Nit), the docs say fixed-width:** still open.\n"
                              "Verdict: findings")),
                 QStringLiteral("0 High, 0 Medium, 0 Low, 0 Nit"));
    }

    // A summary of counts, or a severity with nothing under it, is no finding.
    void summariesAndEmptySeveritiesAreNoFindings() {
        using verdict_line::count;
        for (const char* reply : {"High: 0, Medium: 1, Low: 2\nVerdict: findings",
                                  "- High: 0\n- Medium: 1\n- Low: 2\nVerdict: findings",
                                  "- **High**: không có.\n- **Medium**: none\nVerdict: findings"})
            QVERIFY2(!count(QString::fromUtf8(reply)).any(), reply);
        // Words after the severity are a finding, in either language.
        QVERIFY(count("- **Low**: không có test cho trang.\nVerdict: findings").low == 1);
    }

    void anIdleLeaderReadsABurstAsOneDigestAfterTheQuietPeriod() {
        start();
        const QString first = task("Cart");
        const QString second = task("Api");
        QVERIFY(teams_->assignTask(team_, first, "dev1", "lead").ok());
        QVERIFY(teams_->assignTask(team_, second, "dev2", "lead").ok());
        host_.messages.clear();
        QVERIFY(teams_->moveTask(team_, first, state::blocked, "dev1", "Which currency?").ok());
        QVERIFY(teams_->moveTask(team_, second, state::blocked, "dev2", "Which route?").ok());
        QVERIFY(host_.messages.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(host_.of("digest").size(), 1, 2000);
        const QString body = host_.of("digest").first().body;
        QVERIFY(body.indexOf("T-1") < body.indexOf("T-2"));
        QVERIFY(body.contains("Which currency?") && body.contains("Which route?"));
        settle();
        QCOMPARE(host_.of("digest").size(), 1);
    }

    void pauseHoldsEveryTurnAndResumeReleasesThemInOrder() {
        start();
        const QString first = task("Cart");
        const QString second = task("Api");
        QVERIFY(teams_->update(team_, {{"paused", true}}, "user").ok());
        QVERIFY(teams_->assignTask(team_, first, "dev1", "lead").ok());
        QVERIFY(teams_->assignTask(team_, second, "dev2", "lead").ok());
        QVERIFY(teams_->moveTask(team_, first, state::blocked, "dev1", "Which currency?").ok());
        settle();
        QVERIFY(host_.messages.isEmpty());
        QCOMPARE(team().held.size(), 2);
        // Held messages survive a restart.
        reopen();
        QCOMPARE(team().held.size(), 2);
        QVERIFY(teams_->update(team_, {{"paused", false}}, "user").ok());
        QCOMPARE(host_.messages.size(), 2);
        QCOMPARE(host_.messages.at(0).toSession, QStringLiteral("dev1"));
        QCOMPARE(host_.messages.at(1).toSession, QStringLiteral("dev2"));
        QVERIFY(team().held.isEmpty());
        // The question asked while paused reaches the leader after the quiet period.
        QTRY_COMPARE_WITH_TIMEOUT(host_.of("digest").size(), 1, 2000);
    }

    void aHeldMessageToAMemberThatLeftIsDropped() {
        start();
        Member third{.sessionId = "dev3",
                     .role = role::developer,
                     .name = "Dev 3",
                     .lane = "Ui",
                     .paths = {"ui"}};
        host_.add("dev3");
        QVERIFY(teams_->addMember(team_, third, "user").ok());
        const QString id = task("Cart");
        QVERIFY(teams_->update(team_, {{"paused", true}}, "user").ok());
        QVERIFY(teams_->assignTask(team_, id, "dev3", "lead").ok());
        QVERIFY(teams_->removeMember(team_, "dev3", "user").ok());
        QVERIFY(teams_->update(team_, {{"paused", false}}, "user").ok());
        QVERIFY(host_.of("assignment").isEmpty());
    }

    void aUsedUpBudgetPausesOnceAndContinuesAfterTheUser() {
        start(2);
        QStringList ids;
        for (int index = 0; index < 4; ++index)
            ids << task(QStringLiteral("Task %1").arg(index));
        QVERIFY(teams_->assignTask(team_, ids.at(0), "dev1", "lead").ok());
        QVERIFY(teams_->assignTask(team_, ids.at(1), "dev2", "lead").ok());
        QCOMPARE(host_.messages.size(), 2);
        QVERIFY(teams_->assignTask(team_, ids.at(2), "dev1", "lead").ok());
        QVERIFY(teams_->assignTask(team_, ids.at(3), "dev2", "lead").ok());
        QCOMPARE(host_.messages.size(), 2);
        QVERIFY(team().paused);
        QCOMPARE(team().pausedReason, QStringLiteral("budget"));
        QCOMPARE(team().held.size(), 2);
        int budget = 0;
        for (const Decision& decision : team().decisions)
            budget += decision.kind == "budget" && decision.state == "open" ? 1 : 0;
        QCOMPARE(budget, 1);
        QCOMPARE(team().decisions.first().choices.first(),
                 QStringLiteral("Continue (2 more turns)"));
        // The user continues: the count starts again and the held turns run in order.
        QVERIFY(teams_->update(team_, {{"paused", false}}, "user").ok());
        QCOMPARE(host_.messages.size(), 4);
        QCOMPARE(host_.messages.at(2).taskId, ids.at(2));
        QCOMPARE(host_.messages.at(3).taskId, ids.at(3));
        QCOMPARE(team().automaticTurns, 2);
        QCOMPARE(team().decisions.first().state, QStringLiteral("withdrawn"));
        QVERIFY(!team().paused);
    }
    void theLeadersTurnEndSendsTheWaitingDigestEndToEnd() {
        agent_test::Fixture fixture;
        QVERIFY(fixture.open(true));
        const QString repository = fixture.directory.filePath("shop");
        QVERIFY(QDir().mkpath(repository));
        for (const QStringList& arguments :
             {QStringList{"init", "-q", "-b", "integration"},
              QStringList{"-c", "user.email=t@example.com", "-c", "user.name=T", "commit", "-q",
                          "--allow-empty", "-m", "start"}}) {
            QProcess process;
            process.setWorkingDirectory(repository);
            process.start("git", arguments);
            QVERIFY(process.waitForFinished(15000) && process.exitCode() == 0);
        }
        const auto command = [&](const QString& type, const QJsonObject& payload) {
            return fixture.host->handleCommand(agent_test::request(type, payload), "desktop-test");
        };
        const QJsonObject created = command(
            "team.create",
            {{"repository", repository},
             {"name", "Shop"},
             {"integration_branch", "integration"},
             {"plan", QJsonObject{{"leader", QJsonObject{{"provider", "claude"}}},
                                  {"developers", QJsonObject{{"count", 1}, {"provider", "codex"}}},
                                  {"reviewer", QJsonObject{{"provider", "claude"}}}}}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(agent_test::text(created)));
        const QJsonArray members = created.value("team").toObject().value("members").toArray();
        const QString teamId = created.value("team").toObject().value("id").toString();
        const QString leader = members.at(0).toObject().value("session_id").toString();
        const QString developer = members.at(1).toObject().value("session_id").toString();
        QVERIFY(command("team.task.create", {{"team_id", teamId}, {"title", "Cart"}})
                    .value("ok")
                    .toBool());
        QVERIFY(command("team.task.assign",
                        {{"team_id", teamId}, {"task_id", "T-1"}, {"owner", developer}})
                    .value("ok")
                    .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.count(developer, "turn.completed") >= 1, 8000);
        QVERIFY(fixture
                    .agents(developer, "agents.team_task_update",
                            {{"task_id", "T-1"}, {"state", "in_progress"}})
                    .value("ok")
                    .toBool());
        // An actionable question waits; the quiet period (20 s) has not passed.
        QVERIFY(fixture
                    .agents(developer, "agents.team_task_update",
                            {{"task_id", "T-1"}, {"state", "blocked"}, {"question", "EUR?"}})
                    .value("ok")
                    .toBool());
        // Any turn of the leader ends with the digest.
        QVERIFY(fixture.agents(developer, "agents.send", {{"to", "leader"}, {"text", "Hello"}})
                    .value("ok")
                    .toBool());
        const auto digests = [&] {
            int count = 0;
            for (const auto& message : fixture.journal->agentMessagesOf(leader, 1, 20))
                count += message.kind == QStringLiteral("digest") ? 1 : 0;
            return count;
        };
        QTRY_COMPARE_WITH_TIMEOUT(digests(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.count(leader, "turn.completed") >= 2, 10000);
        QCOMPARE(digests(), 1);
    }
};

QTEST_GUILESS_MAIN(TeamFlowTest)
#include "TeamFlowTest.moc"
