// #468: the transcript draws the daemon's team messages as blocks by reading their text
// (agent/TeamTranscriptBlocks). This runs the real TeamCoordinator on a fake runtime and reads
// every message it sends, so a change to one of its texts fails here instead of silently
// turning a block back into plain text.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "agent/TeamTranscriptBlocks.h"

#include <QStandardPaths>
#include <QTest>

using namespace ketplus::daemon;
using namespace team_test;

namespace {

QVariantMap blockOf(const TeamMessage& message) {
    return ketplus::team_blocks::fromMessage({{"kind", message.kind}, {"body", message.body}});
}

} // namespace

class TeamTranscriptContractTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    const Team& team() { return *teams_->team(team_); }

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
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
    }

    void everyTeamMessageReadsAsItsBlock() {
        QString id;
        QVERIFY(teams_
                    ->createTask(team_,
                                 {.title = "Cart",
                                  .description = "One page from cart to receipt.",
                                  .lane = "Web",
                                  .criteria = {"Adds an item", "Shows the total"}},
                                 "lead", &id)
                    .ok());
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QCOMPARE(host_.of("assignment").size(), 1);
        const QVariantMap assigned = blockOf(host_.of("assignment").first());
        QCOMPARE(assigned.value("kind").toString(), QStringLiteral("assignment"));
        QCOMPARE(assigned.value("team").toString(), QStringLiteral("Checkout"));
        QCOMPARE(assigned.value("taskId").toString(), id);
        QCOMPARE(assigned.value("title").toString(), QStringLiteral("Cart"));
        QVERIFY(assigned.value("facts").toStringList().contains("Lane Web"));
        QVERIFY(assigned.value("facts").toStringList().contains("Assigned by Lead"));
        QCOMPARE(assigned.value("text").toString(),
                 QStringLiteral("One page from cart to receipt."));
        QCOMPARE(assigned.value("criteria").toList().size(), 2);

        // The handover carries its reason.
        QVERIFY(teams_->handoverTask(team_, id, "dev2", "Dev 1 is on another task", "user").ok());
        const QVariantMap handed = blockOf(host_.of("assignment").last());
        QCOMPARE(handed.value("kind").toString(), QStringLiteral("assignment"));
        QVERIFY(handed.value("text").toString().startsWith("Reason: Dev 1 is on another task"));
        QVERIFY(teams_->handoverTask(team_, id, "dev1", "Back to Dev 1", "user").ok());

        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inReview, "dev1").ok());
        QCOMPARE(host_.of("review").size(), 1);
        const QVariantMap request = blockOf(host_.of("review").first());
        QCOMPARE(request.value("kind").toString(), QStringLiteral("review"));
        QCOMPARE(request.value("taskId").toString(), id);
        QVERIFY(request.value("facts").toStringList().first().startsWith("Round 1 of"));
        QCOMPARE(request.value("criteria").toList().size(), 2);

        teams_->reviewFinished("rev", "web/cart.txt:3 high: the total is wrong.\nVerdict: findings",
                               false);
        const QVariantMap verdict = blockOf(host_.of("review").last());
        QCOMPARE(verdict.value("kind").toString(), QStringLiteral("verdict"));
        QCOMPARE(verdict.value("taskId").toString(), id);
        QVERIFY(verdict.value("text").toString().contains("the total is wrong"));
        QVERIFY(!verdict.value("text").toString().contains("Fix them"));

        // The leader's digest: one block, grouped by task.
        host_.changed.insert(id, {"web/cart.cpp", "api/routes.cpp"});
        teams_->memberTurnEnded("dev1");
        host_.messages.clear();
        teams_->memberTurnEnded("lead");
        QCOMPARE(host_.of("digest").size(), 1);
        const QVariantMap digest = blockOf(host_.of("digest").first());
        QCOMPARE(digest.value("kind").toString(), QStringLiteral("digest"));
        QVERIFY(digest.value("changes").toInt() > 0);
        bool needsYou = false;
        for (const QVariant& group : digest.value("groups").toList())
            for (const QVariant& line : group.toMap().value("lines").toList())
                needsYou = needsYou || line.toMap().value("needsYou").toBool();
        QVERIFY(needsYou);

        // An answered decision reaches the leader and the blocked owner.
        QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", "Which currency?").ok());
        QString decision;
        QVERIFY(teams_
                    ->raiseQuestion(team_,
                                    {{"question", "Which currency?"},
                                     {"choices", QJsonArray{"EUR", "USD"}},
                                     {"recommended", "EUR"},
                                     {"task_id", id}},
                                    "lead", &decision)
                    .ok());
        host_.messages.clear();
        QVERIFY(teams_->answerDecision(team_, decision, "EUR", "user").ok());
        QCOMPARE(host_.of("decision").size(), 2);
        for (const TeamMessage& message : host_.of("decision")) {
            const QVariantMap answered = blockOf(message);
            QCOMPARE(answered.value("kind").toString(), QStringLiteral("decision"));
            QCOMPARE(answered.value("question").toString(), QStringLiteral("Which currency?"));
            QCOMPARE(answered.value("answer").toString(), QStringLiteral("EUR"));
            QCOMPARE(answered.value("taskId").toString(), id);
        }
        QVERIFY(team().decision(decision));
    }
};

QTEST_GUILESS_MAIN(TeamTranscriptContractTest)
#include "TeamTranscriptContractTest.moc"
