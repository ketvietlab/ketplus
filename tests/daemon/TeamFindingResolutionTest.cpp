#include "TeamFakeHost.h"
#include "storage/LocalJournal.h"
#include "teams/TeamCoordinator.h"

#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

using namespace ketplus::daemon;
using namespace ketplus::daemon::teams;
using namespace team_test;

class TeamFindingResolutionTest final : public QObject {
    Q_OBJECT
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};
    QString id_;
    const QString baseline_{40, QLatin1Char('a')};
    const QString fix_{40, QLatin1Char('c')};
    const QString receiptId_{64, QLatin1Char('b')};
    const Task& task() const { return *teams_->team(team_)->task(id_); }
    QString encoded(const QJsonObject& j) const {
        return QString::fromUtf8(QJsonDocument(j).toJson(QJsonDocument::Compact));
    }
    QString findings(const QString& severity = "p2", const QString& impact = "behavior") const {
        return "```ketplus-findings-v1\n" +
               encoded({{"version", 1},
                        {"findings", QJsonArray{QJsonObject{{"id", "f1"},
                                                            {"severity", severity},
                                                            {"impact", impact},
                                                            {"scope", "task"},
                                                            {"evidence", QString(64, 'e')},
                                                            {"close_when", "prose_line_width_80"},
                                                            {"mandatory_rule", false}}}}}) +
               "\n```\nVerdict: findings";
    }
    QJsonObject request(const QString& action = "resolve_findings") const {
        QJsonObject j{{"version", 1},
                      {"action", action},
                      {"expected_team_revision", teams_->team(team_)->revision},
                      {"expected_task_revision", task().changedRevision},
                      {"head_commit", fix_}};
        if (action == "resolve_findings") {
            j["evidence_id"] = receiptId_;
            j["confidence"] = 99;
            j["resolutions"] = QJsonArray{
                QJsonObject{{"id", "f1"}, {"status", "fixed"}, {"reason", "bounded_fix_verified"}}};
        }
        return j;
    }
    void receipt() {
        risk::VerifiedEvidence e;
        e.teamId = team_;
        e.taskId = id_;
        e.taskRevision = task().changedRevision;
        e.id = receiptId_;
        e.revision = fix_;
        e.reviewedRevision = baseline_;
        e.localFixVerified = true;
        for (const QString& domain : risk::mandatoryDomains())
            e.domains[domain] = risk::Signal::Clear;
        e.requiredChecks = {"required_matrix"};
        e.checks = {{"required_matrix", fix_, QString(64, 'f'), true}};
        e.scopeEvidence = QString(64, 'd');
        e.rollbackEvidence = QString(64, 'e');
        e.materialRiskKnown = e.lowRiskVerified = e.userApprovalsSatisfied = true;
        e.unresolvedFindings = e.unresolvedP0 = e.unresolvedP1 = e.unresolvedFailures = 0;
        host_.routingReceipts.insert(receiptId_, e);
    }
    QString decide(const QJsonObject& j, const QString& actor = "lead") {
        return teams_->acceptReview(team_, id_, encoded(j), "bounded_fix_verified", actor)
            .error.value_or(TeamError{})
            .code;
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
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->load();
        teams_->setQuietPeriod(3600000);
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(teams_
                    ->createTask(team_, {.title = "Prose", .lane = "Web", .criteria = {"Verified"}},
                                 "lead", &id_)
                    .ok());
        QVERIFY(teams_->assignTask(team_, id_, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, id_, state::inProgress, "dev1").ok());
        host_.heads[task().branch] = baseline_;
        QVERIFY(teams_
                    ->updateTask(team_, id_,
                                 {{"met", QJsonArray{QJsonObject{{"index", 0}, {"met", true}}}}},
                                 "dev1")
                    .ok());
        QVERIFY(teams_->moveTask(team_, id_, state::inReview, "dev1").ok());
        finishReview(*teams_, "rev", findings(), false);
        QCOMPARE(task().state, state::inProgress);
        QVERIFY(task().reviewFindings && !task().reviewFindings->legacy);
        host_.heads[task().branch] = fix_;
        receipt();
    }
    void leaderVerificationIsFencedAndDoesNotForgeAReviewerVerdict() {
        const auto j = request();
        QCOMPARE(decide(j, "dev1"), "role_forbidden");
        QCOMPARE(decide(j, "rev"), "role_forbidden");
        QVERIFY(decide(j).isEmpty());
        QCOMPARE(task().state, state::ready);
        QCOMPARE(task().headCommit, fix_);
        QCOMPARE(task().reviewFindings->revision, baseline_);
        QVERIFY(!review_policy::blocks(*task().reviewFindings));
        int resolved = 0;
        for (const auto& h : task().history) {
            QVERIFY(h.kind != "review_accepted");
            resolved += h.kind == "findings_resolved";
        }
        QCOMPARE(resolved, 1);
        QVERIFY(!decide(j).isEmpty());
        QVERIFY(task().state != state::done);
    }
    void nonblockingFindingsDoNotForgeANoFindingsVerdict() {
        QVERIFY(teams_->moveTask(team_, id_, state::inReview, "dev1").ok());
        finishReview(*teams_, "rev", findings("nit", "preference"), false);
        QCOMPARE(task().state, state::ready);
        QVERIFY(task().reviewFindings && !review_policy::blocks(*task().reviewFindings));
        QString verdict;
        for (const auto& h : task().history)
            if (h.kind == "verdict")
                verdict = h.data.value("verdict").toString();
        QCOMPARE(verdict, "findings");
    }
    void aContradictoryNoFindingsReplyCannotMakeP0Ready() {
        QVERIFY(teams_->moveTask(team_, id_, state::inReview, "dev1").ok());
        finishReview(*teams_, "rev",
                     findings("p0").replace("Verdict: findings", "Verdict: no findings"), true);
        QCOMPARE(task().state, state::blocked);
        QVERIFY(task().reviewFindings && review_policy::blocks(*task().reviewFindings));
        receipt();
        QCOMPARE(decide(request()), "review_required");
    }
    void incompleteAcceptanceOrCallerRevisionCannotResolve() {
        QVERIFY(teams_
                    ->updateTask(team_, id_,
                                 {{"met", QJsonArray{QJsonObject{{"index", 0}, {"met", false}}}}},
                                 "dev1")
                    .ok());
        receipt();
        QCOMPARE(decide(request()), "acceptance_incomplete");
        auto stale = request();
        stale["expected_task_revision"] = task().changedRevision - 1;
        QCOMPARE(decide(stale), "stale_revision");
        stale = request();
        stale["expected_team_revision"] = teams_->team(team_)->revision - 1;
        QCOMPARE(decide(stale), "stale_revision");
    }
    void missingFailedHighRiskOrStaleEvidenceCannotResolve() {
        host_.routingReceipts.clear();
        QCOMPARE(decide(request()), "evidence_missing");
        receipt();
        host_.routingReceipts[receiptId_].checks[0].passed = false;
        QCOMPARE(decide(request()), "review_required");
        receipt();
        host_.routingReceipts[receiptId_].domains["authentication"] = risk::Signal::RequiresReview;
        QCOMPARE(decide(request()), "review_required");
        receipt();
        host_.routingReceipts[receiptId_].reviewedRevision = fix_;
        QCOMPARE(decide(request()), "review_required");
        receipt();
        host_.heads[task().branch] = baseline_;
        QCOMPARE(decide(request()), "stale_revision");
        QCOMPARE(task().state, state::inProgress);
    }
    void twoRoundsRequireExplicitSinglePassArbitrationAndCannotReplay() {
        QVERIFY(teams_->moveTask(team_, id_, state::inReview, "dev1").ok());
        finishReview(*teams_, "rev", findings("p1"), false);
        QCOMPARE(task().state, state::blocked);
        QCOMPARE(task().round, 2);
        QVERIFY(!teams_->moveTask(team_, id_, state::inReview, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id_, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id_, state::inReview, "dev1").ok());
        QCOMPARE(task().state, state::blocked);
        QCOMPARE(task().round, 2);
        const auto j = request("request_review");
        QVERIFY(decide(j).isEmpty());
        QCOMPARE(task().round, 3);
        QCOMPARE(task().state, state::inReview);
        QVERIFY(!decide(j).isEmpty());
        finishReview(*teams_, "rev", findings("p1"), false);
        QCOMPARE(task().state, state::blocked);
        QCOMPARE(task().round, 3);
        QVERIFY(!teams_->moveTask(team_, id_, state::inReview, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id_, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id_, state::inReview, "dev1").ok());
        QCOMPARE(task().state, state::blocked);
        QCOMPARE(task().round, 3);
    }
};
QTEST_GUILESS_MAIN(TeamFindingResolutionTest)
#include "TeamFindingResolutionTest.moc"
