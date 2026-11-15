// T-28: a team command is answered before its slow consequences run. The change is stored
// before the reply; pointing members at their worktrees and sending their messages follow
// it, in order, once, also across a restart. Git here is a wrapper that takes 2 s a call.
#include "AgentTestFixture.h"
#include "storage/LocalJournal.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

#include <memory>
#include <utility>

using namespace ketplus;
using namespace ketplus::daemon;

namespace {

struct SlowGit final {
    QByteArray path = qgetenv("PATH");
    bool installed = false;

    // Two seconds by default. The session runtime gives up on a git call after two seconds,
    // so a worktree switch is only carried out with a shorter delay.
    explicit SlowGit(const QString& directory, const QByteArray& seconds = "2",
                     const QString& startedFile = {}) {
        const QString real = QStandardPaths::findExecutable(QStringLiteral("git"));
        QFile script(directory + QStringLiteral("/git"));
        if (real.isEmpty() || !QDir().mkpath(directory) || !script.open(QIODevice::WriteOnly))
            return;
        script.write("#!/bin/sh\n");
        if (!startedFile.isEmpty())
            script.write(": > '" + startedFile.toUtf8().replace("'", "'\\''") + "'\n");
        script.write("sleep " + seconds + "\nexec '" + real.toUtf8() + "' \"$@\"\n");
        script.close();
        installed = script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("PATH", directory.toUtf8() + ':' + path);
    }
    ~SlowGit() { qputenv("PATH", path); }
    SlowGit(const SlowGit&) = delete;
    SlowGit& operator=(const SlowGit&) = delete;
};

constexpr qint64 replyBudgetMs = 100;

} // namespace

class TeamReplyTest final : public QObject {
    Q_OBJECT

    std::unique_ptr<agent_test::Fixture> fixture_;
    QString teamId_;
    QString leader_;
    QStringList developers_;

    QJsonObject command(const QString& type, const QJsonObject& payload) {
        return fixture_->host->handleCommand(agent_test::request(type, payload), "desktop-test");
    }

    // Sends a command as the desktop does and measures until its reply.
    QJsonObject timed(const QString& type, const QJsonObject& payload, qint64* elapsed) {
        QJsonObject reply;
        QElapsedTimer clock;
        clock.start();
        fixture_->host->handleCommandDeferred(agent_test::request(type, payload), "desktop-test",
                                              [&](const QJsonObject& result) {
                                                  reply = result;
                                                  *elapsed = clock.elapsed();
                                              });
        if (reply.isEmpty())
            (void)QTest::qWaitFor([&] { return !reply.isEmpty(); }, 60000);
        return reply;
    }

    // Sends a command through the event loop, as a client's socket delivers it, and measures
    // the wait before it is handled and the time until its reply.
    QJsonObject queued(const QString& type, const QJsonObject& payload, qint64* waited,
                       qint64* total) {
        QJsonObject reply;
        QElapsedTimer clock;
        clock.start();
        QTimer::singleShot(0, [&] {
            *waited = clock.elapsed();
            fixture_->host->handleCommandDeferred(agent_test::request(type, payload),
                                                  "desktop-test", [&](const QJsonObject& result) {
                                                      reply = result;
                                                      *total = clock.elapsed();
                                                  });
        });
        (void)QTest::qWaitFor([&] { return !reply.isEmpty(); }, 60000);
        return reply;
    }

    // The team messages of `kind` the developers received.
    int count(const QString& kind) const {
        int total = 0;
        for (const QString& developer : developers_)
            for (const auto& message : fixture_->journal->agentMessagesOf(developer, 1, 50))
                total += message.kind == kind ? 1 : 0;
        return total;
    }

    void assignByTheLeader(const QString& taskId, const QString& developer) {
        QJsonObject reply;
        fixture_->host->handleAgentCommandDeferred(
            fixture_->host->agentToken(leader_), "agents.team_task_assign",
            {{"task_id", taskId}, {"owner", fixture_->number(developer)}},
            [&](const QJsonObject& result) { reply = result; });
        QTRY_VERIFY_WITH_TIMEOUT(!reply.isEmpty(), 30000);
        QVERIFY2(reply.value("ok").toBool(), qPrintable(agent_test::text(reply)));
    }

    // Where a conversation stands, and how many times it was switched, as the journal has it.
    std::pair<QString, qint64> standing(const QString& sessionId) const {
        const auto stored = fixture_->journal->readSessions();
        if (stored)
            for (const auto& session : *stored)
                if (session.id == sessionId)
                    return {QFileInfo(session.worktreePath).canonicalFilePath(),
                            session.contextRevision};
        return {};
    }

    QJsonObject team() {
        return command("team.get", {{"team_id", teamId_}}).value("team").toObject();
    }

    // T-32: Start, then the first developer's conversation stands in T-1's worktree with its
    // assignment turn over. Returns that worktree.
    QString startedInItsWorktree() {
        const QString plan = openDecision("first_plan");
        if (!command("team.decision.answer",
                     {{"team_id", teamId_}, {"decision_id", plan}, {"choice", "Start"}})
                 .value("ok")
                 .toBool())
            return {};
        QString worktree;
        for (const QJsonValue& value : team().value("tasks").toArray())
            if (value.toObject().value("id").toString() == QStringLiteral("T-1"))
                worktree =
                    QFileInfo(value.toObject().value("worktree").toString()).canonicalFilePath();
        const QString developer = developers_.at(0);
        if (worktree.isEmpty() ||
            !QTest::qWaitFor([&] { return standing(developer).first == worktree; }, 30000) ||
            !QTest::qWaitFor(
                [&] { return fixture_->count(developer, QStringLiteral("turn.completed")) > 0; },
                30000))
            return {};
        QTest::qWait(200);
        return worktree;
    }

    QJsonObject handover(qint64* waited, qint64* total) {
        return queued("team.task.handover",
                      {{"team_id", teamId_},
                       {"task_id", "T-1"},
                       {"owner", developers_.at(1)},
                       {"reason", "Developer 1 is on the API"}},
                      waited, total);
    }

    // The task says nothing of a previous owner that could not leave.
    bool noWaitingMove() {
        const QJsonObject current = team();
        if (!current.value("relocations").toArray().isEmpty())
            return false;
        for (const QJsonValue& value : current.value("tasks").toArray())
            for (const QJsonValue& entry : value.toObject().value("history").toArray())
                if (entry.toObject().value("text").toString().contains("when its turn ends"))
                    return false;
        return true;
    }

    QString openDecision(const QString& kind) {
        const QJsonObject team =
            command("team.get", {{"team_id", teamId_}}).value("team").toObject();
        for (const QJsonValue& value : team.value("decisions").toArray())
            if (value.toObject().value("kind").toString() == kind &&
                value.toObject().value("state").toString() == QStringLiteral("open"))
                return value.toObject().value("id").toString();
        return {};
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // A team with two developers whose first plan (T-1, T-2) waits for the user's Start.
    void init() {
        fixture_ = std::make_unique<agent_test::Fixture>();
        QVERIFY(fixture_->open(true));
        const QString repository = fixture_->directory.filePath("shop");
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
        const QJsonObject created = command(
            "team.create",
            {{"repository", repository},
             {"name", "Shop"},
             {"integration_branch", "integration"},
             {"plan", QJsonObject{{"leader", QJsonObject{{"provider", "claude"}}},
                                  {"developers", QJsonObject{{"count", 2}, {"provider", "claude"}}},
                                  {"reviewer", QJsonObject{{"provider", "claude"}}}}}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(agent_test::text(created)));
        const QJsonObject team = created.value("team").toObject();
        teamId_ = team.value("id").toString();
        const QJsonArray members = team.value("members").toArray();
        leader_ = members.at(0).toObject().value("session_id").toString();
        developers_ = {members.at(1).toObject().value("session_id").toString(),
                       members.at(2).toObject().value("session_id").toString()};
        for (const QString& title : {"Cart", "Api"})
            QVERIFY(command("team.task.create", {{"team_id", teamId_}, {"title", title}})
                        .value("ok")
                        .toBool());
        assignByTheLeader("T-1", developers_.at(0));
        assignByTheLeader("T-2", developers_.at(1));
        QCOMPARE(count("assignment"), 0);
        QVERIFY(!openDecision("first_plan").isEmpty());
    }

    void cleanup() { fixture_.reset(); }

    // Criteria 2 and 3: Start is answered at once, with the decision answered in the reply;
    // the assignments follow in their order.
    void startIsAnsweredBeforeTheAssignmentsAreSent() {
        const QString plan = openDecision("first_plan");
        const SlowGit slow(fixture_->directory.filePath("bin"));
        QVERIFY(slow.installed);
        qint64 elapsed = -1;
        const QJsonObject reply =
            timed("team.decision.answer",
                  {{"team_id", teamId_}, {"decision_id", plan}, {"choice", "Start"}}, &elapsed);
        QVERIFY2(reply.value("ok").toBool(), qPrintable(agent_test::text(reply)));
        qInfo("team.decision.answer (first plan, Start): reply after %lld ms", elapsed);
        QVERIFY2(elapsed < replyBudgetMs, qPrintable(QString::number(elapsed)));
        const QJsonObject team = reply.value("team").toObject();
        QVERIFY(team.value("plan_approved").toBool());
        for (const QJsonValue& value : team.value("decisions").toArray())
            if (value.toObject().value("id").toString() == plan)
                QCOMPARE(value.toObject().value("state").toString(), QStringLiteral("answered"));
        QVERIFY(!team.contains("outbox"));
        QCOMPARE(count("assignment"), 0);
        QTRY_COMPARE_WITH_TIMEOUT(count("assignment"), 2, 30000);
        QCOMPARE(fixture_->journal->agentMessagesOf(developers_.at(0), 1, 50)
                     .first()
                     .body.contains("T-1"),
                 true);
        QCOMPARE(fixture_->journal->agentMessagesOf(developers_.at(1), 1, 50)
                     .first()
                     .body.contains("T-2"),
                 true);
    }

    // Criterion 4: the daemon stops between the reply and the assignments. The next start
    // sends each of them once.
    void aRestartAfterTheReplySendsTheAssignmentsOnce() {
        const QString plan = openDecision("first_plan");
        qint64 elapsed = -1;
        const QJsonObject reply =
            timed("team.decision.answer",
                  {{"team_id", teamId_}, {"decision_id", plan}, {"choice", "Start"}}, &elapsed);
        QVERIFY(reply.value("ok").toBool());
        // No event has run since the reply: the consequences are only stored.
        fixture_->restart(true);
        QTRY_COMPARE_WITH_TIMEOUT(count("assignment"), 2, 30000);
        QTest::qWait(500);
        fixture_->restart(true);
        QTest::qWait(500);
        QCOMPARE(count("assignment"), 2);
    }

    // Criterion 5: a pause sent while the assignments are still out is answered at once; what
    // Start released before it is still sent, and refusals are decided before the reply.
    void aSecondCommandIsAnsweredWithoutWaiting() {
        const QString plan = openDecision("first_plan");
        const SlowGit slow(fixture_->directory.filePath("bin"));
        QVERIFY(slow.installed);
        qint64 elapsed = -1;
        QVERIFY(timed("team.decision.answer",
                      {{"team_id", teamId_}, {"decision_id", plan}, {"choice", "Start"}}, &elapsed)
                    .value("ok")
                    .toBool());
        const QJsonObject paused =
            timed("team.update", {{"team_id", teamId_}, {"paused", true}}, &elapsed);
        QVERIFY2(paused.value("ok").toBool(), qPrintable(agent_test::text(paused)));
        qInfo("team.update (pause): reply after %lld ms", elapsed);
        QVERIFY2(elapsed < replyBudgetMs, qPrintable(QString::number(elapsed)));
        const QJsonObject again =
            timed("team.decision.answer",
                  {{"team_id", teamId_}, {"decision_id", plan}, {"choice", "Start"}}, &elapsed);
        QCOMPARE(again.value("error_code").toString(), QStringLiteral("decision_closed"));
        QVERIFY2(elapsed < replyBudgetMs, qPrintable(QString::number(elapsed)));
        QTRY_COMPARE_WITH_TIMEOUT(count("assignment"), 2, 30000);
        // Resuming releases nothing that was sent already, and is answered at once too.
        const QJsonObject resumed =
            timed("team.update", {{"team_id", teamId_}, {"paused", false}}, &elapsed);
        QVERIFY(resumed.value("ok").toBool());
        qInfo("team.update (resume): reply after %lld ms", elapsed);
        QVERIFY2(elapsed < replyBudgetMs, qPrintable(QString::number(elapsed)));
        QTest::qWait(500);
        QCOMPARE(count("assignment"), 2);
    }

    // Criteria 5 and 6: once the outbox has started a member's worktree switch, its git runs
    // off the daemon's thread: the event loop keeps turning, a next command is answered at
    // once, and the assignments still arrive in their order, each once.
    void theOutboxGitHoldsUpNothing() {
        const QString plan = openDecision("first_plan");
        // Half a second per call: a switch's two calls block a synchronous daemon for a second,
        // and stay well inside the runtime's two-second git limit on a busy machine.
        const SlowGit slow(fixture_->directory.filePath("bin"), "0.5");
        QVERIFY(slow.installed);
        qint64 elapsed = -1;
        QVERIFY(timed("team.decision.answer",
                      {{"team_id", teamId_}, {"decision_id", plan}, {"choice", "Start"}}, &elapsed)
                    .value("ok")
                    .toBool());
        QElapsedTimer loop;
        loop.start();
        QTest::qWait(50); // The outbox starts its first switch here.
        qInfo("event loop: 50 ms wait took %lld ms while the outbox's git runs", loop.elapsed());
        QVERIFY2(loop.elapsed() < 400, qPrintable(QString::number(loop.elapsed())));
        QCOMPARE(count("assignment"), 0);
        // Commands sent through the event loop while the switches' git runs (two entries of
        // two calls of half a second each), with the members' turns that Start set off.
        for (int index = 0; index < 5; ++index) {
            qint64 waited = -1;
            qint64 total = -1;
            const QJsonObject created =
                queued("team.task.create",
                       {{"team_id", teamId_}, {"title", QStringLiteral("Search %1").arg(index)}},
                       &waited, &total);
            QVERIFY2(created.value("ok").toBool(), qPrintable(agent_test::text(created)));
            qInfo("team.task.create %d during the outbox's git: waited %lld ms, replied after "
                  "%lld ms, %d assignments sent",
                  index, waited, total, count("assignment"));
            QVERIFY2(total < replyBudgetMs, qPrintable(QString::number(total)));
            QTest::qWait(100);
        }
        QTRY_COMPARE_WITH_TIMEOUT(count("assignment"), 2, 60000);
        const auto first = fixture_->journal->agentMessagesOf(developers_.at(0), 1, 50);
        const auto second = fixture_->journal->agentMessagesOf(developers_.at(1), 1, 50);
        QString t1;
        QString t2;
        for (const auto& message : first)
            if (message.kind == QStringLiteral("assignment"))
                t1 = message.createdAt;
        for (const auto& message : second)
            if (message.kind == QStringLiteral("assignment"))
                t2 = message.createdAt;
        QVERIFY2(!t1.isEmpty() && t1 <= t2, qPrintable(t1 + " " + t2));
        // Each owner's conversation stands in its task's worktree.
        const QJsonObject team =
            command("team.get", {{"team_id", teamId_}}).value("team").toObject();
        const auto stored = fixture_->journal->readSessions();
        QVERIFY(stored);
        int pointed = 0;
        for (const QJsonValue& value : team.value("tasks").toArray()) {
            const QJsonObject task = value.toObject();
            for (const auto& session : *stored)
                if (session.id == task.value("owner").toString() &&
                    !task.value("worktree").toString().isEmpty() &&
                    QFileInfo(session.worktreePath).canonicalFilePath() ==
                        QFileInfo(task.value("worktree").toString()).canonicalFilePath())
                    ++pointed;
        }
        QVERIFY2(pointed == 2, QJsonDocument(team.value("tasks").toArray()).toJson().constData());
        QTest::qWait(500);
        QCOMPARE(count("assignment"), 2);
    }

    // T-32, criteria 1 to 3: the previous owner stands in the handed-over worktree. The reply
    // comes before its switch out asks git; meanwhile the event loop turns and a next command
    // is answered at once; then it stands in the repository, switched once.
    void aHandoverIsAnsweredBeforeThePreviousOwnerLeaves() {
        const QString worktree = startedInItsWorktree();
        QVERIFY(!worktree.isEmpty());
        const QString previous = developers_.at(0);
        const qint64 revision = standing(previous).second;
        // Half a second a call: the synchronous switch held the reply for a second.
        const SlowGit slow(fixture_->directory.filePath("bin"), "0.5");
        QVERIFY(slow.installed);
        qint64 waited = -1;
        qint64 total = -1;
        const QJsonObject reply = handover(&waited, &total);
        QVERIFY2(reply.value("ok").toBool(), qPrintable(agent_test::text(reply)));
        qInfo("team.task.handover: waited %lld ms, replied after %lld ms", waited, total);
        QVERIFY2(total < replyBudgetMs, qPrintable(QString::number(total)));
        QCOMPARE(standing(previous).first, worktree);
        QElapsedTimer loop;
        loop.start();
        QTest::qWait(50);
        qInfo("event loop: 50 ms wait took %lld ms while the switch's git runs", loop.elapsed());
        QVERIFY2(loop.elapsed() < 400, qPrintable(QString::number(loop.elapsed())));
        const QJsonObject created = queued(
            "team.task.create", {{"team_id", teamId_}, {"title", "Search"}}, &waited, &total);
        QVERIFY(created.value("ok").toBool());
        qInfo("team.task.create during the switch's git: waited %lld ms, replied after %lld ms",
              waited, total);
        QVERIFY2(total < replyBudgetMs, qPrintable(QString::number(total)));
        const QString repository =
            QFileInfo(fixture_->directory.filePath("shop")).canonicalFilePath();
        QTRY_COMPARE_WITH_TIMEOUT(standing(previous).first, repository, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(noWaitingMove(), 10000);
        // The new owner stands in the worktree, and the old one was switched once.
        QTRY_COMPARE_WITH_TIMEOUT(standing(developers_.at(1)).first, worktree, 30000);
        QTest::qWait(500);
        QCOMPARE(standing(previous).second, revision + 1);
    }

    // T-32, criterion 3: the daemon stops between the reply and the switch out. The next
    // start carries out the stored move, once.
    void aRestartBeforeThePreviousOwnerLeftMovesItOnce() {
        const QString worktree = startedInItsWorktree();
        QVERIFY(!worktree.isEmpty());
        const QString previous = developers_.at(0);
        const qint64 revision = standing(previous).second;
        qint64 waited = -1;
        qint64 total = -1;
        {
            const SlowGit slow(fixture_->directory.filePath("bin"), "0.5");
            QVERIFY(slow.installed);
            QVERIFY(handover(&waited, &total).value("ok").toBool());
            QCOMPARE(team().value("relocations").toArray().size(), 1);
            // No switch has been stored since the reply: the move is only stored.
            fixture_->restart(true);
        }
        QCOMPARE(standing(previous).first, worktree);
        const QString repository =
            QFileInfo(fixture_->directory.filePath("shop")).canonicalFilePath();
        QTRY_COMPARE_WITH_TIMEOUT(standing(previous).first, repository, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(noWaitingMove(), 10000);
        fixture_->restart(true);
        QTest::qWait(500);
        QCOMPARE(standing(previous).first, repository);
        QCOMPARE(standing(previous).second, revision + 1);
    }

    // A message held while the team was paused is released by resume after the reply.
    void resumeIsAnsweredBeforeTheHeldMessagesAreSent() {
        qint64 elapsed = -1;
        QVERIFY(timed("team.decision.answer",
                      {{"team_id", teamId_},
                       {"decision_id", openDecision("first_plan")},
                       {"choice", "Start"}},
                      &elapsed)
                    .value("ok")
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(count("assignment"), 2, 30000);
        QVERIFY(
            command("team.update", {{"team_id", teamId_}, {"paused", true}}).value("ok").toBool());
        QVERIFY(command("team.task.create", {{"team_id", teamId_}, {"title", "Docs"}})
                    .value("ok")
                    .toBool());
        assignByTheLeader("T-3", developers_.at(0));
        QTest::qWait(300);
        QCOMPARE(count("assignment"), 2);
        const QString gitStarted = fixture_->directory.filePath("resume-git-started");
        const SlowGit slow(fixture_->directory.filePath("bin"), "2", gitStarted);
        QVERIFY(slow.installed);
        QJsonObject resumed;
        int assignmentsAtReply = -1;
        bool gitStartedAtReply = true;
        fixture_->host->handleCommandDeferred(
            agent_test::request("team.update", {{"team_id", teamId_}, {"paused", false}}),
            "desktop-test", [&](const QJsonObject& result) {
                assignmentsAtReply = count("assignment");
                gitStartedAtReply = QFileInfo::exists(gitStarted);
                resumed = result;
            });
        QTRY_VERIFY_WITH_TIMEOUT(!resumed.isEmpty(), 30000);
        QVERIFY2(resumed.value("ok").toBool(), qPrintable(agent_test::text(resumed)));
        // ADR 0114 orders the reply before deferred Git and message delivery. Wall time
        // also includes the journal commit and scheduler delays, so observe that order.
        QCOMPARE(assignmentsAtReply, 2);
        QVERIFY2(!gitStartedAtReply, "Deferred Git ran before the resume reply.");
        QVERIFY(!resumed.value("team").toObject().value("paused").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(gitStarted), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(count("assignment"), 3, 30000);
    }
};

QTEST_GUILESS_MAIN(TeamReplyTest)
#include "TeamReplyTest.moc"
