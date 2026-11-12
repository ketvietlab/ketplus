// T-28: a team command is answered before its slow consequences run. The change is stored
// before the reply; pointing members at their worktrees and sending their messages follow
// it, in order, once, also across a restart. Git here is a wrapper that takes 2 s a call.
#include "AgentTestFixture.h"
#include "storage/LocalJournal.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>

#include <memory>

using namespace ketplus;
using namespace ketplus::daemon;

namespace {

struct SlowGit final {
    QByteArray path = qgetenv("PATH");
    bool installed = false;

    explicit SlowGit(const QString& directory) {
        const QString real = QStandardPaths::findExecutable(QStringLiteral("git"));
        QFile script(directory + QStringLiteral("/git"));
        if (real.isEmpty() || !QDir().mkpath(directory) || !script.open(QIODevice::WriteOnly))
            return;
        script.write("#!/bin/sh\nsleep 2\nexec '" + real.toUtf8() + "' \"$@\"\n");
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
            QTest::qWaitFor([&] { return !reply.isEmpty(); }, 60000);
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
        const SlowGit slow(fixture_->directory.filePath("bin"));
        QVERIFY(slow.installed);
        const QJsonObject resumed =
            timed("team.update", {{"team_id", teamId_}, {"paused", false}}, &elapsed);
        QVERIFY2(resumed.value("ok").toBool(), qPrintable(agent_test::text(resumed)));
        qInfo("team.update (resume with a held assignment): reply after %lld ms", elapsed);
        QVERIFY2(elapsed < replyBudgetMs, qPrintable(QString::number(elapsed)));
        QVERIFY(!resumed.value("team").toObject().value("paused").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(count("assignment"), 3, 30000);
    }
};

QTEST_GUILESS_MAIN(TeamReplyTest)
#include "TeamReplyTest.moc"
