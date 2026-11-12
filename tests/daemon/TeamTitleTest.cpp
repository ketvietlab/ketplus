// T-26: a team member's conversation keeps its member name. Its turns start from the envelope
// of an agent message ("[KetPlus agent message]"), which earlier builds took for a title when
// Codex had no name for the thread. Run with QT_QPA_PLATFORM=offscreen.
#include "AgentTestFixture.h"
#include "agents/AgentInstructions.h"
#include "storage/LocalJournal.h"

#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>

#include <memory>

using namespace ketplus;
using namespace ketplus::daemon;

class TeamTitleTest final : public QObject {
    Q_OBJECT

    std::unique_ptr<agent_test::Fixture> fixture_;
    QString teamId_;
    QString leader_;
    QString developer_;

    QJsonObject command(const QString& type, const QJsonObject& payload,
                        const QString& sessionId = {}) {
        return fixture_->host->handleCommand(
            agent_test::request(type, payload, sessionId, sessionId.isEmpty() ? -1 : 1),
            "desktop-test");
    }

    QString title(const QString& sessionId) const {
        for (const StoredSession& session : fixture_->journal->sessions())
            if (session.id == sessionId)
                return session.title;
        return {};
    }

    // A message from the leader starts one more turn of the developer; waits for its end and
    // for the title read that follows it.
    void oneMoreTurn(const QString& text) {
        const int before = fixture_->count(developer_, "turn.completed");
        const QJsonObject sent = fixture_->agents(
            leader_, "agents.send", {{"to", fixture_->number(developer_)}, {"text", text}});
        QVERIFY2(sent.value("ok").toBool(), qPrintable(agent_test::text(sent)));
        QTRY_VERIFY_WITH_TIMEOUT(fixture_->count(developer_, "turn.completed") > before, 8000);
        QTest::qWait(300);
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

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
                                  {"developers", QJsonObject{{"count", 1}, {"provider", "codex"}}},
                                  {"reviewer", QJsonObject{{"provider", "claude"}}}}}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(agent_test::text(created)));
        const QJsonObject team = created.value("team").toObject();
        teamId_ = team.value("id").toString();
        leader_ = team.value("members").toArray().at(0).toObject().value("session_id").toString();
        developer_ =
            team.value("members").toArray().at(1).toObject().value("session_id").toString();
        QCOMPARE(title(developer_), QStringLiteral("Developer 1"));
    }

    void cleanup() { fixture_.reset(); }

    void aMemberKeepsItsNameThroughItsTurns() {
        QVERIFY(command("team.task.create", {{"team_id", teamId_}, {"title", "Cart"}})
                    .value("ok")
                    .toBool());
        QVERIFY(command("team.task.assign",
                        {{"team_id", teamId_}, {"task_id", "T-1"}, {"owner", developer_}})
                    .value("ok")
                    .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(fixture_->count(developer_, "turn.completed") >= 1, 8000);
        QTest::qWait(300);
        QCOMPARE(title(developer_), QStringLiteral("Developer 1"));
        oneMoreTurn("Hello");
        QCOMPARE(title(developer_), QStringLiteral("Developer 1"));
        for (const QJsonObject& renamed : fixture_->payloads(developer_, "session.renamed"))
            QVERIFY(!agent_messaging::isDeliveryText(renamed.value("title").toString()));
    }

    void aUserRenameSurvivesTurnsAndARestart() {
        oneMoreTurn("Start");
        const QJsonObject renamed = command("session.rename", {{"title", "Cart work"}}, developer_);
        QVERIFY2(renamed.value("ok").toBool(), qPrintable(agent_test::text(renamed)));
        oneMoreTurn("Again");
        QCOMPARE(title(developer_), QStringLiteral("Cart work"));
        fixture_->restart(true);
        QCOMPARE(title(developer_), QStringLiteral("Cart work"));
        oneMoreTurn("After the restart");
        QCOMPARE(title(developer_), QStringLiteral("Cart work"));
    }

    // A member an earlier build titled from its first agent message gets its name back when
    // the daemon loads the team, as an automatic rename with the event's usual shape.
    void aWronglyTitledMemberIsRepairedOnLoad() {
        StoredSession stored;
        for (const StoredSession& session : fixture_->journal->sessions())
            if (session.id == developer_)
                stored = session;
        stored.title = QStringLiteral("[KetPlus agent message]");
        QVERIFY(fixture_->journal->upsertSession(stored));
        fixture_->restart(true);
        QCOMPARE(title(developer_), QStringLiteral("Developer 1"));
        const QJsonObject last = fixture_->payloads(developer_, "session.renamed").last();
        QCOMPARE(last, (QJsonObject{{"title", "Developer 1"}, {"custom_title", false}}));
        // A member titled otherwise is left as it is.
        const QString before = title(leader_);
        fixture_->restart(true);
        QCOMPARE(title(leader_), before);
    }

    // Outside a team, a conversation an agent message reaches keeps the title it had, and the
    // envelope's marker never reads as one.
    void aConversationOutsideTheTeamIsNotTitledByTheEnvelope() {
        const QString plain = fixture_->start("codex", "Fix the cart");
        QVERIFY(!plain.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(fixture_->count(plain, "turn.completed") >= 1, 8000);
        QTest::qWait(300);
        QCOMPARE(title(plain), QStringLiteral("Fix the cart"));
        const QJsonObject sent = fixture_->agents(
            leader_, "agents.send", {{"to", fixture_->number(plain)}, {"text", "Totals?"}});
        QVERIFY2(sent.value("ok").toBool(), qPrintable(agent_test::text(sent)));
        QTRY_VERIFY_WITH_TIMEOUT(fixture_->count(plain, "turn.completed") >= 2, 8000);
        QTest::qWait(300);
        QCOMPARE(title(plain), QStringLiteral("Fix the cart"));
        QVERIFY(agent_messaging::isDeliveryText(QStringLiteral("[KetPlus agent message]")));
        QVERIFY(agent_messaging::isDeliveryText(
            QStringLiteral("  [KetPlus agent message]\nFrom conversation #12.")));
        QVERIFY(!agent_messaging::isDeliveryText(QStringLiteral("Fix the [KetPlus] bug")));
    }
};

QTEST_GUILESS_MAIN(TeamTitleTest)
#include "TeamTitleTest.moc"
