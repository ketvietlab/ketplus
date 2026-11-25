// ADR 0114 §7 (T-9): what the leader's own session did is context in its digest, never a reason
// to start one of its turns and never marked [needs you]. The user's, the daemon's, a
// developer's and a reviewer's updates stay actionable. The coordinator runs on a real journal
// with a fake runtime; no provider is started.
#include "TeamFakeHost.h"
#include "storage/LocalJournal.h"

#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

using namespace ketplus::daemon;
using namespace team_test;

namespace {

class DigestCoordinator final : public TeamCoordinator {
  public:
    using TeamCoordinator::mutableTeam;
    using TeamCoordinator::TeamCoordinator;
};

} // namespace

class TeamDigestActorTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<DigestCoordinator> teams_;
    qint64 team_{0};

    const Team& team() { return *teams_->team(team_); }

    QString newTask(const QString& title, const QString& owner, const QString& actor = "lead") {
        QString id;
        if (!teams_
                 ->createTask(team_, {.title = title, .lane = "Web", .criteria = {"Works"}}, actor,
                              &id)
                 .ok() ||
            (!owner.isEmpty() && !teams_->assignTask(team_, id, owner, actor).ok()))
            return {};
        return id;
    }
    // The leader's end of turn is what sends a digest of a leader that is idle.
    void leaderTurnEnds() {
        teams_->memberTurnEnded("lead");
        QTest::qWait(30);
    }
    // Everything the leader did so far is digested, so a test starts from a quiet team.
    void settle() {
        leaderTurnEnds();
        host_.messages.clear();
    }
    [[nodiscard]] int actionableBy(const QString& actor) {
        int count = 0;
        for (const auto& value : teams_->updatesJson(team_)) {
            const QJsonObject update = value.toObject();
            count += update.value("actor").toString() == actor &&
                             update.value("actionable").toBool() &&
                             update.value("kind").toString() != "wait"
                         ? 1
                         : 0;
        }
        return count;
    }
    // A digest that was sent to `session`, whole.
    [[nodiscard]] QString digestTo(const QString& session) const {
        for (const TeamMessage& message : host_.messages)
            if (message.kind == "digest" && message.toSession == session)
                return message.body;
        return {};
    }
    [[nodiscard]] static QStringList linesWith(const QString& body, const QString& needle) {
        QStringList result;
        for (const QString& line : body.split('\n'))
            if (line.contains(needle))
                result.append(line);
        return result;
    }

    // What the leader does by itself: change criteria, create and assign a task, and close the
    // blocked task of dev1 by hand, which is the actionable "→ done" update of ADR 0114 §7.
    // The next digest without a real reason must still be silent.
    // dev1 keeps another task and dev2 gets one, so closing leaves nobody idle and the goal open:
    // those consequences are the daemon's, and are tested apart.
    void leaderWorks(const QString& id) {
        QVERIFY(!newTask("Search", "dev1").isEmpty());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", "Which endpoint?").ok());
        settle();
        QVERIFY(teams_->setCriteria(team_, id, {"Works", "Is fast"}, "lead").ok());
        QVERIFY(teams_->closeTask(team_, id, "abcdef1234567", "Dev 2 chose the endpoint.", "lead")
                    .ok());
        QVERIFY(!newTask("Billing", "dev2").isEmpty());
        QVERIFY2(actionableBy("lead") > 0, "The fixture must make an actionable update.");
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        teams_.reset();
        journal_.reset();
        QFile::remove(directory_.filePath("journal.sqlite3"));
        host_ = {};
        for (const QString& id : {"lead", "dev1", "dev2", "dev3"})
            host_.add(id);
        host_.add("rev", "claude");
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        teams_ = std::make_unique<DigestCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(60 * 60 * 1000);
        teams_->load();
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
        host_.messages.clear();
    }

    void repeatedQuestionOnlyWakesOnce() {
        const QString id = newTask("Cart", "dev1");
        QVERIFY(!id.isEmpty());
        const int initialTurns = team().automaticTurns;
        for (const QString& question : {"Which endpoint?", "Which  endpoint?", "Which endpoint?"}) {
            QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
            QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", question).ok());
            leaderTurnEnds();
        }
        QCOMPARE(host_.of("digest").size(), 1);
        QCOMPARE(team().automaticTurns, initialTurns + 1);

        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", "Which region?").ok());
        leaderTurnEnds();
        QCOMPARE(host_.of("digest").size(), 2);
        QCOMPARE(team().automaticTurns, initialTurns + 2);

        QVERIFY(teams_->setCriteria(team_, id, {"Works in both regions"}, "lead").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", "Which endpoint?").ok());
        leaderTurnEnds();
        QCOMPARE(host_.of("digest").size(), 3);
        QCOMPARE(team().automaticTurns, initialTurns + 3);
    }

    void completingTheTeamKeepsUpdatesForTheNextDigest() {
        const QString id = newTask("Cart", "dev1");
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        const int turns = team().automaticTurns;
        QVERIFY(teams_->closeTask(team_, id, "abcdef1234567", "Finished.", "lead").ok());
        leaderTurnEnds();
        QVERIFY(host_.of("digest").isEmpty());
        QCOMPARE(team().automaticTurns, turns);
        QCOMPARE(teams_->teamJson(team()).value("quiesced_reason").toString(), "tasks_done");

        const QString other = newTask("Billing", "dev2");
        QVERIFY(!other.isEmpty());
        QVERIFY(team().quiescedReason.isEmpty());
        QVERIFY(teams_->moveTask(team_, other, state::inProgress, "dev2").ok());
        QVERIFY(teams_->moveTask(team_, other, state::blocked, "dev2", "Which region?").ok());
        leaderTurnEnds();
        const QString body = digestTo("lead");
        QVERIFY(body.contains("Every task is done"));
        QVERIFY(body.contains("[needs you]"));
        QVERIFY(body.contains("Which region?"));
    }

    void developerMergeConflictReopensAQuiescedTeamOnce() {
        const QString id = newTask("Cart", "dev1");
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", "When does it ship?").ok());
        QCOMPARE(team().quiescedReason, QString{});
        settle();
        QVERIFY(teams_->wait(team_, "dev1", 30, "deployment", id).ok());
        QVERIFY(team().quiescedReason.isEmpty());
        QVERIFY(teams_->wait(team_, "lead", 30, "deployment", id).ok());
        QCOMPARE(team().quiescedReason, "waiting_for_external_event");
        const auto& task = *team().task(id);
        QVERIFY(toJson(task).contains("external_wait_question"));
        QVERIFY(!toJson(task, false).contains("external_wait_question"));
        const int turns = team().automaticTurns;
        leaderTurnEnds();
        QVERIFY(host_.of("digest").isEmpty());
        QCOMPARE(team().automaticTurns, turns);

        teams_->userMessageReceived("dev1");
        QCOMPARE(team().quiescedReason, "waiting_for_external_event");
        teams_->userMessageReceived("rev");
        QCOMPARE(team().quiescedReason, "waiting_for_external_event");
        teams_->userMessageReceived("lead");
        QVERIFY(team().quiescedReason.isEmpty());
        leaderTurnEnds();
        QCOMPARE(team().quiescedReason, "waiting_for_external_event");
        teams_->setWaitMinuteForTesting(1);
        QVERIFY(teams_->wait(team_, "lead", 1, "deployment").ok());
        QTRY_COMPARE(host_.of("wait").size(), 1);
        QVERIFY(team().quiescedReason.isEmpty());
        QCOMPARE(team().automaticTurns, turns + 1);
        leaderTurnEnds();
        QCOMPARE(team().quiescedReason, "waiting_for_external_event");

        const QString conflict = "Merge conflict in Cart.cpp. Which change should I keep?";
        const int turnsBeforeConflict = team().automaticTurns;
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", conflict).ok());
        leaderTurnEnds();
        QCOMPARE(host_.of("digest").size(), 1);
        QCOMPARE(team().automaticTurns, turnsBeforeConflict + 1);
        QVERIFY(digestTo("lead").contains(conflict));
        QVERIFY(team().quiescedReason.isEmpty());

        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", conflict).ok());
        leaderTurnEnds();
        QCOMPARE(host_.of("digest").size(), 1);
        QCOMPARE(team().automaticTurns, turnsBeforeConflict + 1);
    }

    void manualClosureStaysActionableInTheJournalWhenTheTeamQuiesces() {
        const QString id = newTask("Cart", "dev1");
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->closeTask(team_, id, "abcdef1234567", "Closed by hand.", "lead").ok());
        QCOMPARE(team().quiescedReason, "tasks_done");
        const auto updates = journal_->teamUpdates(team_, true, 100);
        const auto closure = std::find_if(updates.cbegin(), updates.cend(), [&](const auto& entry) {
            return entry.taskId == id && entry.data.value("completion").toString() == "closed";
        });
        QVERIFY(closure != updates.cend());
        QVERIFY(closure->actionable);
    }

    void newDecisionReopensAQuiescedTeamForTheUsersAnswer() {
        const QString id = newTask("Cart", "dev1");
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->closeTask(team_, id, "abcdef1234567", "Finished.", "lead").ok());
        settle();
        QCOMPARE(team().quiescedReason, "tasks_done");

        QString decision;
        QVERIFY(teams_
                    ->raiseQuestion(
                        team_,
                        {{"question", "Which currency?"}, {"choices", QJsonArray{"EUR", "USD"}}},
                        "lead", &decision)
                    .ok());
        QVERIFY(team().quiescedReason.isEmpty());
        QCOMPARE(team().openDecisions(), 1);
        QVERIFY(teams_->answerDecision(team_, decision, "EUR", "user").ok());
        QTRY_COMPARE(host_.of("decision").size(), 1);
        QCOMPARE(host_.of("decision").first().toSession, "lead");
    }

    void reviewFindingsClearAStaleQuiesceLockAndWakeTheLeader() {
        const QString id = newTask("Cart", "dev1");
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inReview, "dev1").ok());
        settle();

        // An in-review task normally prevents quiescence. Inject a stale marker to
        // exercise the verdict handler's recovery rather than opening a real review in it.
        QVERIFY(team().quiescedReason.isEmpty());
        teams_->mutableTeam(team_)->quiescedReason = "waiting_for_external_event";
        finishReview(*teams_, "rev", "web/cart.txt:3 high: wrong.\nVerdict: findings", false);
        QVERIFY(team().quiescedReason.isEmpty());
        leaderTurnEnds();
        QCOMPARE(host_.of("digest").size(), 1);
        QVERIFY(digestTo("lead").contains("Verdict on"));
    }

    // Criterion 1: only the leader's own updates, with or without a wait: no digest, no turn.
    void theLeadersOwnUpdatesAloneSendNoDigest() {
        const QString id = newTask("Cart", "dev1");
        QVERIFY(!id.isEmpty());
        leaderWorks(id);
        host_.messages.clear();
        QVERIFY(teams_->wait(team_, "lead", 30, "the user's answer").ok());
        const int turns = team().automaticTurns;
        leaderTurnEnds();
        QVERIFY2(host_.of("digest").isEmpty(), "The leader's own actions start no digest.");
        QCOMPARE(team().automaticTurns, turns);
    }

    // Criterion 2: they are not lost. The next digest with something real lists them, without
    // [needs you], beside the real line which has it.
    void theyRideAlongWithTheNextRealDigest() {
        const QString id = newTask("Cart", "dev1");
        leaderWorks(id);
        host_.messages.clear();
        leaderTurnEnds();
        QVERIFY(host_.of("digest").isEmpty());

        // The other developer asks a question: a real reason.
        const QString other = team().tasks.last().id;
        QVERIFY(teams_->moveTask(team_, other, state::inProgress, "dev2").ok());
        QVERIFY(teams_->moveTask(team_, other, state::blocked, "dev2", "Which endpoint?").ok());
        leaderTurnEnds();
        const QString body = digestTo("lead");
        QVERIFY2(!body.isEmpty(), "A developer's question is a real reason.");
        const QStringList real = linesWith(body, "Which endpoint?");
        QVERIFY2(!real.isEmpty(), qPrintable(body));
        QVERIFY(real.first().contains("[needs you]"));
        // What the leader closed by hand is listed, without the mark.
        const QStringList closed = linesWith(body, "closed by");
        QCOMPARE(closed.size(), 1);
        QVERIFY2(!closed.first().contains("[needs you]"), qPrintable(body));
        // The leader's own task creation is listed as an ordinary line.
        const QStringList created = linesWith(body, "Billing");
        QVERIFY2(!created.isEmpty(), qPrintable(body));
        for (const QString& line : created)
            QVERIFY2(!line.contains("[needs you]"), qPrintable(line));
        // And it is digested now: the next end of turn repeats nothing.
        host_.messages.clear();
        leaderTurnEnds();
        QVERIFY(host_.of("digest").isEmpty());
    }

    // Criterion 3: the user's, a developer's, the daemon's and a reviewer's updates still send
    // a digest and still carry [needs you], also right after something the leader did.
    void everyOtherActorStillNeedsTheLeader_data() {
        QTest::addColumn<QString>("actor");
        for (const char* actor : {"developer", "user", "reviewer"})
            QTest::newRow(actor) << QString::fromLatin1(actor);
    }
    void everyOtherActorStillNeedsTheLeader() {
        QFETCH(QString, actor);
        const QString id = newTask("Cart", "dev1");
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        settle();
        // The leader's own action comes first and sends nothing alone.
        QVERIFY(teams_->setCriteria(team_, id, {"Works", "Is fast"}, "lead").ok());
        leaderTurnEnds();
        QVERIFY(host_.of("digest").isEmpty());

        QString needle;
        if (actor == "developer") {
            // A task blocked with its question.
            QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", "Which endpoint?").ok());
            needle = "Which endpoint?";
        } else if (actor == "user") {
            QVERIFY(teams_->moveTask(team_, id, state::blocked, "user", "Waiting for design").ok());
            needle = "Waiting for design";
        } else {
            // A review verdict: findings go back to the developer and the leader reads a line.
            QVERIFY(teams_->moveTask(team_, id, state::inReview, "dev1").ok());
            settle();
            finishReview(*teams_, "rev", "web/cart.txt:3 high: wrong.\nVerdict: findings", false);
            needle = "Verdict on";
        }
        leaderTurnEnds();
        const QString body = digestTo("lead");
        QVERIFY2(!body.isEmpty(), "Another actor's update must still start a digest.");
        const QStringList lines = linesWith(body, needle);
        QVERIFY2(!lines.isEmpty(), qPrintable(body));
        QVERIFY2(lines.first().contains("[needs you]"), qPrintable(body));
    }

    // What the daemon works out inside the leader's own command is the daemon's update, not the
    // leader's: closing a developer's last task leaves it idle, and closing a dependency makes
    // a reviewed task ready to merge. The
    // digest still goes out and still marks them.
    void whatTheDaemonDerivesFromTheLeadersCommandStillNeedsTheLeader_data() {
        QTest::addColumn<QString>("what");
        QTest::addColumn<QString>("needle");
        QTest::newRow("idle developer") << "idle" << "has no task left";
        QTest::newRow("merge offer") << "merge" << "can be merged";
    }
    void whatTheDaemonDerivesFromTheLeadersCommandStillNeedsTheLeader() {
        QFETCH(QString, what);
        QFETCH(QString, needle);
        const QString first = newTask("Cart", "dev1");
        QVERIFY(teams_->moveTask(team_, first, state::inProgress, "dev1").ok());
        if (what == "idle") {
            // dev2 keeps a task, so only dev1 runs out of work.
            const QString other = newTask("Billing", "dev2");
            QVERIFY(teams_->moveTask(team_, other, state::inProgress, "dev2").ok());
        } else if (what == "merge") {
            // dev1 keeps a task; Billing waits for Cart, reviewed and ready.
            QVERIFY(!newTask("Search", "dev1").isEmpty());
            const QString second = newTask("Billing", "dev2");
            QVERIFY(teams_->moveTask(team_, second, state::inProgress, "dev2").ok());
            QVERIFY(teams_->updateTask(team_, second, {{"depends_on", QJsonArray{first}}}, "lead")
                        .ok());
            QVERIFY(teams_->moveTask(team_, second, state::inReview, "dev2").ok());
            finishReview(*teams_, "rev", "Verdict: no findings", true);
            QCOMPARE(team().task(second)->state, state::ready);
        }
        settle();
        QVERIFY(teams_->closeTask(team_, first, "abcdef1234567", "Merged by hand.", "lead").ok());
        leaderTurnEnds();
        const QString body = digestTo("lead");
        QVERIFY2(!body.isEmpty(), "The daemon's consequence of a leader command starts a digest.");
        const QStringList lines = linesWith(body, needle);
        QVERIFY2(!lines.isEmpty(), qPrintable(body));
        QVERIFY2(lines.first().contains("[needs you]"), qPrintable(body));
        // The leader's own closing stays an ordinary line.
        const QStringList closed = linesWith(body, "closed by");
        QCOMPARE(closed.size(), 1);
        QVERIFY2(!closed.first().contains("[needs you]"), qPrintable(body));
    }

    // Criterion 3, a merge seen outside the app is the daemon's.
    void aMergeSeenOutsideTheAppStillNeedsTheLeader() {
        const QString id = newTask("Cart", "dev1");
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inReview, "dev1").ok());
        finishReview(*teams_, "rev", "Fine.\nVerdict: no findings", true);
        settle();
        host_.merged.insert(id);
        teams_->checkMerged(team_);
        leaderTurnEnds();
        const QString body = digestTo("lead");
        QVERIFY2(!body.isEmpty(), "A merge seen outside the app starts a digest.");
        const QStringList lines = linesWith(body, "integration contains it");
        QVERIFY2(!lines.isEmpty(), qPrintable(body));
        QCOMPARE(lines.filter("[needs you]").size(), 1);
    }

    // Criterion 4: the filter follows the leader at the time of sending. The former leader's
    // updates are a member's after a handover, so the new leader is told of them.
    void aNewLeaderIsToldOfTheFormerLeadersUpdates() {
        const QString id = newTask("Cart", "dev1");
        leaderWorks(id);
        host_.messages.clear();
        leaderTurnEnds();
        QVERIFY(host_.of("digest").isEmpty());

        teams_->conversationDeleted("lead");
        QVERIFY(teams_
                    ->addMember(team_,
                                {.sessionId = "dev3", .role = role::leader, .name = "Lead 2"},
                                "user")
                    .ok());
        host_.messages.clear();
        teams_->memberTurnEnded("dev3");
        QTest::qWait(30);
        const QString body = digestTo("dev3");
        QVERIFY2(!body.isEmpty(), "The former leader's updates are the new leader's to read.");
        QVERIFY2(body.contains("[needs you]"), qPrintable(body));
    }
};

QTEST_GUILESS_MAIN(TeamDigestActorTest)
#include "TeamDigestActorTest.moc"
