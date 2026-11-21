// ADR 0115: what a client reads about a team is bounded by the size of the team, never by how
// long it ran, and a message too large for one frame never costs the client its connection.
// A team far heavier than any real one is stored, loaded and read through every client form.
#include "TeamFakeHost.h"
#include "protocol/DaemonProtocol.h"
#include "storage/LocalJournal.h"

#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace ketplus::daemon;
using namespace team_test;

namespace {

qsizetype bytes(const QJsonObject& json) {
    return QJsonDocument(json).toJson(QJsonDocument::Compact).size();
}

qsizetype bytes(const QJsonArray& json) {
    return QJsonDocument(json).toJson(QJsonDocument::Compact).size();
}

QString filler(const int size, const QChar letter) { return QString(size, letter); }

constexpr int kTasks = 200;
constexpr int kHistory = 40;
constexpr int kDecisions = 300;
constexpr int kHeld = 60;
constexpr int kUpdates = 400;

} // namespace

class TeamPayloadTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    // Every task with a long description, the largest history entries the bounds allow and
    // 200 outside paths; hundreds of decisions, held messages and Updates.
    static Team heavyTeam() {
        Team team = standardTeam();
        QStringList outside;
        for (int index = 0; index < maximumOutsidePaths; ++index)
            outside.append(
                QStringLiteral("api/generated/module-%1/a-long-file-name.ts").arg(index));
        QJsonArray paths;
        for (int index = 0; index < maximumHistoryListItems; ++index)
            paths.append(outside.at(index));
        for (int index = 1; index <= kTasks; ++index) {
            Task task;
            task.id = QStringLiteral("T-%1").arg(index);
            task.title = QStringLiteral("Task %1").arg(index);
            task.description = filler(1500, u'd');
            task.lane = QStringLiteral("Web");
            task.owner = QStringLiteral("dev1");
            task.state = state::inProgress;
            task.criteria = {{.text = filler(200, u'c')}, {.text = filler(200, u'c'), .met = true}};
            task.outsideLane = outside;
            for (int entry = 0; entry < kHistory; ++entry)
                task.history.append(
                    {.at = nowIso(),
                     .actor = QStringLiteral("daemon"),
                     .kind = QStringLiteral("outside_lane"),
                     .text = filler(int(maximumHistoryText), u'h'),
                     .data = {{QStringLiteral("paths"), paths}, {QStringLiteral("added"), paths}}});
            team.tasks.append(task);
        }
        team.nextTask = kTasks + 1;
        for (int index = 1; index <= kDecisions; ++index)
            team.decisions.append(
                {.id = QStringLiteral("D-%1").arg(index),
                 .question = filler(300, u'q'),
                 .context = filler(1500, u'x'),
                 .choices = {QStringLiteral("Yes"), QStringLiteral("No")},
                 .raisedBy = QStringLiteral("lead"),
                 .raisedAt = nowIso(),
                 .state =
                     index > kDecisions - 5 ? QStringLiteral("open") : QStringLiteral("answered"),
                 .answer = index > kDecisions - 5 ? QString{} : QStringLiteral("Yes")});
        team.nextDecision = kDecisions + 1;
        for (int index = 0; index < kHeld; ++index)
            team.held.append(QJsonObject{{QStringLiteral("kind"), QStringLiteral("digest")},
                                         {QStringLiteral("body"), filler(20000, u'b')}});
        return team;
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
        const Team team = heavyTeam();
        const auto id = journal_->insertTeam(team.repository, toJson(team), team.memberSessions());
        QVERIFY(id);
        team_ = *id;
        for (int index = 0; index < kUpdates; ++index)
            QVERIFY(journal_->appendTeamUpdate({.teamId = team_,
                                                .at = nowIso(),
                                                .actor = QStringLiteral("daemon"),
                                                .kind = QStringLiteral("outside_lane"),
                                                .taskId = QStringLiteral("T-1"),
                                                .text = filler(5000, u'u'),
                                                .data = {}}));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(60 * 60 * 1000);
        teams_->load();
        QVERIFY(teams_->team(team_));
    }

    void theStoredDocumentIsFarLargerThanAFrame() {
        QVERIFY(bytes(toJson(*teams_->team(team_))) > 10 * protocol::maximumFrameBytes);
    }

    // team.list and team.changed: the members, never the work.
    void theDirectoryFormFollowsTheMembers() {
        const QJsonObject directory = teams_->directoryJson(*teams_->team(team_));
        QVERIFY2(bytes(directory) < 16 * 1024, QByteArray::number(bytes(directory)));
        QVERIFY(directory.value("directory").toBool());
        QVERIFY(!directory.contains("tasks"));
        QVERIFY(!directory.contains("decisions"));
        QVERIFY(!directory.contains("held"));
        QCOMPARE(directory.value("tasks_total").toInt(), kTasks);
        QCOMPARE(directory.value("held_count").toInt(), kHeld);
        QCOMPARE(directory.value("members").toArray().size(), 4);
    }

    // team.get: every task without history, the open decisions and the newest answered ones.
    void theSummaryFitsAFrame() {
        const QJsonObject summary = teams_->teamJson(*teams_->team(team_));
        QVERIFY2(bytes(summary) < protocol::maximumFrameBytes, QByteArray::number(bytes(summary)));
        const QJsonArray tasks = summary.value("tasks").toArray();
        QCOMPARE(tasks.size(), kTasks);
        const QJsonObject first = tasks.first().toObject();
        QVERIFY(!first.contains("history"));
        QCOMPARE(first.value("outside_lane").toArray().size(), summaryOutsidePaths);
        QCOMPARE(first.value("outside_lane_total").toInt(), maximumOutsidePaths);
        const QJsonArray decisions = summary.value("decisions").toArray();
        QCOMPARE(decisions.size(), 5 + summaryAnsweredDecisions);
        QCOMPARE(decisions.last().toObject().value("id").toString(),
                 QStringLiteral("D-%1").arg(kDecisions));
        QCOMPARE(summary.value("decisions_total").toInt(), kDecisions);
        QVERIFY(!summary.contains("held"));
    }

    void updatesComePageByPage() {
        const QJsonArray newest = teams_->updatesJson(team_);
        QCOMPARE(newest.size(), defaultPage);
        QVERIFY(bytes(newest) < protocol::maximumFrameBytes / 4);
        const qint64 oldest = newest.last().toObject().value("id").toInteger();
        const QJsonArray older = teams_->updatesJson(team_, maximumPage * 10, oldest);
        QCOMPARE(older.size(), maximumPage);
        QVERIFY(older.first().toObject().value("id").toInteger() < oldest);
        QVERIFY(bytes(older) < protocol::maximumFrameBytes);
    }

    void historyComesPageByPage() {
        const Task& task = teams_->team(team_)->tasks.first();
        const QJsonObject newest = taskPageJson(task, 10);
        QCOMPARE(newest.value("history").toArray().size(), 10);
        QCOMPARE(newest.value("history_total").toInt(), kHistory);
        QCOMPARE(newest.value("history_start").toInt(), kHistory - 10);
        const QJsonObject earlier = taskPageJson(task, 10, kHistory - 10);
        QCOMPARE(earlier.value("history_start").toInt(), kHistory - 20);
        const QJsonObject first = taskPageJson(task, 1000, 5);
        QCOMPARE(first.value("history").toArray().size(), 5);
        QCOMPARE(first.value("history_start").toInt(), 0);
        QVERIFY(bytes(taskPageJson(task, maximumPage)) < protocol::maximumFrameBytes);
    }

    // A reply too large for one frame becomes a failure of that request.
    void anOversizedReplyBecomesAFailureOfThatRequest() {
        const QJsonObject request{{"protocol_version", protocol::currentVersion},
                                  {"message_id", "m-1"},
                                  {"message_type", "team.get"},
                                  {"payload", QJsonObject{}}};
        const QJsonObject reply =
            protocol::response(request, {{"ok", true}, {"team", toJson(*teams_->team(team_))}});
        const QByteArray frame = protocol::encodeFrame(reply);
        QVERIFY(frame.size() > protocol::maximumFrameBytes);
        const QJsonObject replaced = protocol::oversizedReplacement(reply, frame.size());
        QCOMPARE(replaced.value("correlation_id").toString(), QStringLiteral("m-1"));
        QCOMPARE(replaced.value("message_type").toString(), QStringLiteral("team.get.result"));
        const QJsonObject payload = replaced.value("payload").toObject();
        QVERIFY(!payload.value("ok").toBool());
        QCOMPARE(payload.value("error_code").toString(), QStringLiteral("response_too_large"));
        QVERIFY(protocol::encodeFrame(replaced).size() < 4096);
    }

    // An event too large for one frame keeps its ids and says it was cut.
    void anOversizedEventKeepsItsIdsAndSaysSo() {
        const QJsonObject team = toJson(*teams_->team(team_));
        const QJsonObject event{
            {"protocol_version", protocol::currentVersion},
            {"message_id", "e-1"},
            {"message_type", "team.changed"},
            {"payload", QJsonObject{{"team", team}, {"revision", team.value("revision")}}}};
        const QJsonObject replaced =
            protocol::oversizedReplacement(event, protocol::encodeFrame(event).size());
        QCOMPARE(replaced.value("message_type").toString(), QStringLiteral("team.changed"));
        const QJsonObject payload = replaced.value("payload").toObject();
        QVERIFY(payload.value("truncated").toBool());
        QCOMPARE(payload.value("team_id").toString(), QString::number(team_));
        QCOMPARE(payload.value("revision"), team.value("revision"));
        QVERIFY(!payload.contains("team"));
        QVERIFY(protocol::encodeFrame(replaced).size() < 4096);
    }
};

QTEST_GUILESS_MAIN(TeamPayloadTest)
#include "TeamPayloadTest.moc"
