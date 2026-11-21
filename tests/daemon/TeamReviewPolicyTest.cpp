#include "teams/TeamReviewPolicy.h"
#include "agents/ReviewIntegrity.h"
#include "teams/Team.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
using namespace ketplus::daemon;
using namespace ketplus::daemon::teams;

namespace {
Task decodeTask(const QJsonObject& value) {
    return teamFromJson(QJsonObject{{"tasks", QJsonArray{value}}}).tasks.first();
}
QJsonObject finding(const QString& severity = "p2") {
    return {{"id", "f1"},
            {"severity", severity},
            {"impact", "behavior"},
            {"scope", "task"},
            {"evidence", QString(64, 'b')},
            {"close_when", "regression_passes"},
            {"mandatory_rule", false}};
}
QJsonObject report(const QJsonObject& f) { return {{"version", 1}, {"findings", QJsonArray{f}}}; }
} // namespace
class TeamReviewPolicyTest final : public QObject {
    Q_OBJECT
  private slots:
    void repliesPreserveBlockingUnknownAndRejectContradictions() {
        const QString head(40, 'a');
        auto r = review_policy::reply("Old review", head, "rev", 1, true);
        QVERIFY(r && r->legacy && review_policy::blocks(*r));
        QVERIFY(!review_policy::reply("Verdict: no findings", head, "rev", 1, false));
        const QString json =
            QString::fromUtf8(QJsonDocument(report(finding("p0"))).toJson(QJsonDocument::Compact));
        const QString text = "```ketplus-findings-v1\n" + json + "\n```\nVerdict: no findings";
        r = review_policy::reply(text, head, "rev", 1, false);
        QVERIFY(r && !r->legacy && review_policy::blocks(*r));
        QVERIFY(review_policy::reply(text + text, head, "rev", 1, false)->legacy);
        const auto missing = review_policy::reply("Legacy", {}, "rev", 1, true);
        QVERIFY(missing && review_policy::fromJson(review_policy::toJson(*missing)));
    }
    void resolutionRequiresIndependentExactBaselineReceipt() {
        const auto rows = review_policy::report(report(finding()));
        QVERIFY(rows);
        review_policy::Record record{QString(40, 'a'), "rev", 1, false, *rows};
        review_policy::Request req;
        req.action = "resolve_findings";
        req.confidence = 99;
        req.taskRevision = 4;
        req.head = QString(40, 'c');
        req.receipt = QString(64, 'd');
        req.resolutions.append({"f1", "fixed", "regression_verified", {}});
        risk::VerifiedEvidence receipt;
        receipt.teamId = 1;
        receipt.taskId = "T-1";
        receipt.taskRevision = 4;
        receipt.id = req.receipt;
        receipt.revision = req.head;
        receipt.reviewedRevision = record.revision;
        receipt.localFixVerified = true;
        for (const auto& domain : risk::mandatoryDomains())
            receipt.domains[domain] = risk::Signal::Clear;
        receipt.requiredChecks = {"full_required_matrix"};
        receipt.checks.append({"full_required_matrix", req.head, QString(64, 'e'), true});
        receipt.scopeEvidence = QString(64, 'f');
        receipt.rollbackEvidence = QString(64, 'a');
        receipt.materialRiskKnown = receipt.lowRiskVerified = receipt.userApprovalsSatisfied = true;
        receipt.unresolvedFindings = receipt.unresolvedP0 = receipt.unresolvedP1 =
            receipt.unresolvedFailures = 0;
        const QString at = "2026-11-16T00:00:00.000Z";
        const auto resolved = review_policy::resolve(record, req, receipt, "lead", at);
        QVERIFY(resolved && !review_policy::blocks(*resolved));
        QVERIFY(!review_policy::resolve(*resolved, req, receipt, "lead", at)); // replay
        req.confidence = 90;
        QVERIFY(!review_policy::resolve(record, req, receipt, "lead", at));
        req.confidence = 99;
        receipt.localFixVerified = false;
        QVERIFY(!review_policy::resolve(record, req, receipt, "lead", at));
        receipt.localFixVerified = true;
        receipt.reviewedRevision = QString(40, 'b');
        QVERIFY(!review_policy::resolve(record, req, receipt, "lead", at));
        receipt.reviewedRevision = record.revision;
        receipt.checks[0].passed = false;
        QVERIFY(!review_policy::resolve(record, req, receipt, "lead", at));
        receipt.checks[0].passed = true;
        receipt.domains["security"] = risk::Signal::RequiresReview;
        QVERIFY(!review_policy::resolve(record, req, receipt, "lead", at));
        receipt.domains["security"] = risk::Signal::Clear;
        auto defer = record;
        defer.findings[0].scope = "outside_task";
        defer.findings[0].impact = "preference";
        defer.findings[0].closeWhen = "owner_accepts_outside_preference";
        receipt.localFixVerified = false;
        req.head = receipt.revision = record.revision;
        receipt.checks[0].revision = record.revision;
        receipt.requiredChecks << "reviewed_preference_nonblocking";
        receipt.checks.append(
            {"reviewed_preference_nonblocking", record.revision, QString(64, 'b'), true});
        req.resolutions[0] = {"f1", "deferred", "outside_scope", "followup_owner"};
        const auto deferred = review_policy::resolve(defer, req, receipt, "lead", at);
        QVERIFY(deferred && !review_policy::blocks(*deferred));
        defer.findings[0].impact = "required_check";
        QVERIFY(!review_policy::resolve(defer, req, receipt, "lead", at));
        req.resolutions[0] = {"f1", "fixed", "regression_verified", {}};
        record.findings[0].severity = "p1";
        QVERIFY(!review_policy::resolve(record, req, receipt, "lead", at));
    }
    void decisionPayloadDoesNotAcceptCallerRiskOrPassFacts() {
        QJsonObject j{{"version", 1},
                      {"action", "request_review"},
                      {"expected_team_revision", 4},
                      {"expected_task_revision", 3},
                      {"head_commit", QString(40, 'a')}};
        const auto parse = [](const QJsonObject& j) {
            return review_policy::request(
                QString::fromUtf8(QJsonDocument(j).toJson(QJsonDocument::Compact)));
        };
        QVERIFY(parse(j));
        j["expected_task_revision"] = 3.2;
        QVERIFY(!parse(j));
        j["expected_task_revision"] = 3;
        j["passed"] = true;
        QVERIFY(!parse(j));
        j.remove("passed");
        j["evidence_id"] = QString(64, 'a');
        QVERIFY(!parse(j));
        j["action"] = "resolve_findings";
        j["confidence"] = 99;
        j["resolutions"] = QJsonArray{};
        QVERIFY(!parse(j));
        j["resolutions"] = QJsonArray{QJsonObject{{"id", "f1"},
                                                  {"status", "deferred"},
                                                  {"reason", "outside_scope"},
                                                  {"owner", "followup_owner"}}};
        QVERIFY(parse(j));
        j["resolutions"] =
            QJsonArray{j["resolutions"].toArray().first(), j["resolutions"].toArray().first()};
        QVERIFY(!parse(j));
    }
    void missingSourceIsNotProofOfTampering() {
        review_integrity::Snapshot before;
        review_integrity::Snapshot after;
        QVERIFY(review_integrity::sourceUnavailable(review_integrity::changes(before, after)));
        before.local = true;
        before.head = QString(40, 'a');
        before.symbolicRef = "refs/heads/topic";
        before.statusHash = QString(64, 'b');
        const auto missingAfter = review_integrity::changes(before, after);
        QVERIFY(review_integrity::sourceUnavailable(missingAfter));
        QCOMPARE(missingAfter.size(), 1); // No fabricated HEAD/branch change to an empty string.
        after = before;
        QVERIFY(review_integrity::changes(before, after).isEmpty());
        after.head = QString(40, 'c');
        const auto changed = review_integrity::changes(before, after);
        QVERIFY(!review_integrity::sourceUnavailable(changed));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.first().toObject().value("field").toString(), QStringLiteral("head"));
    }
    void priorityAndImpactCannotBeDeferredAway() {
        for (const QString& severity :
             {QStringLiteral("p0"), QStringLiteral("p1"), QStringLiteral("unknown")}) {
            const auto rows = review_policy::report(report(finding(severity)));
            QVERIFY(rows);
            QVERIFY(review_policy::blocks(rows->first()));
            QVERIFY(!review_policy::deferrable(rows->first()));
            QVERIFY(!review_policy::leaderVerifiable(rows->first()));
            auto forged = rows->first();
            forged.status = "fixed";
            QVERIFY(review_policy::blocks(forged));
        }
        for (const QString& impact :
             {QStringLiteral("requirement"), QStringLiteral("required_check"),
              QStringLiteral("unknown")}) {
            auto f = finding();
            f["scope"] = "outside_task";
            f["impact"] = impact;
            const auto rows = review_policy::report(report(f));
            QVERIFY(rows);
            QVERIFY(!review_policy::deferrable(rows->first()));
        }
    }
    void nitIsNonBlockingOnlyWithoutMandatoryImpact() {
        auto f = finding("nit");
        f["impact"] = "preference";
        auto rows = review_policy::report(report(f));
        QVERIFY(rows);
        QVERIFY(!review_policy::blocks(rows->first()));
        f["mandatory_rule"] = true;
        rows = review_policy::report(report(f));
        QVERIFY(rows);
        QVERIFY(review_policy::blocks(rows->first()));
        f["mandatory_rule"] = false;
        f["impact"] = "required_check";
        rows = review_policy::report(report(f));
        QVERIFY(rows);
        QVERIFY(review_policy::blocks(rows->first()));
    }
    void structuredFieldsAreRequiredAndResolutionIsServerOwned() {
        for (const QString& key : finding().keys()) {
            auto f = finding();
            f.remove(key);
            QVERIFY2(!review_policy::report(report(f)), qPrintable(key));
        }
        auto f = finding();
        f["status"] = "deferred";
        QVERIFY(!review_policy::report(report(f)));
        f = finding();
        f["evidence"] = "raw output or a credential-bearing URL";
        QVERIFY(!review_policy::report(report(f)));
        auto j = report(finding());
        j["findings"] = QJsonArray{finding(), finding()};
        QVERIFY(!review_policy::report(j));
        j = report(finding());
        j["version"] = 99;
        QVERIFY(!review_policy::report(j));
    }
    void taskPersistenceKeepsFindingsAndLegacyDefaults() {
        Task task;
        task.id = "T-1";
        QVERIFY(!decodeTask(toJson(task)).reviewFindings);
        const auto rows = review_policy::report(report(finding()));
        QVERIFY(rows);
        task.reviewFindings = review_policy::Record{QString(40, 'a'), "reviewer", 1, false, *rows};
        task.arbitratedRound = 3;
        task.arbitrationHead = QString(40, 'c');
        auto stored = toJson(task);
        const auto loaded = decodeTask(stored);
        QVERIFY(loaded.reviewFindings);
        QCOMPARE(loaded.arbitratedRound, 3);
        QCOMPARE(loaded.arbitrationHead, task.arbitrationHead);
        QCOMPARE(review_policy::toJson(*loaded.reviewFindings),
                 review_policy::toJson(*task.reviewFindings));
        stored["review_findings"] = QJsonObject{{"version", 99}};
        QVERIFY(decodeTask(stored).reviewFindings);
        QVERIFY(review_policy::blocks(*decodeTask(stored).reviewFindings));
    }
    void resolutionsRoundTripAndMalformedOnesFailClosed() {
        auto f = finding();
        f["scope"] = "outside_task";
        const auto rows = review_policy::report(report(f));
        QVERIFY(rows);
        review_policy::Record record{QString(40, 'a'), "reviewer", 2, false, *rows};
        auto& closed = record.findings[0];
        closed.status = "deferred";
        closed.actor = "leader";
        closed.reason = "outside_accepted_scope";
        closed.owner = "developer";
        closed.resolvedRevision = QString(40, 'c');
        closed.receipt = QString(64, 'd');
        closed.at = "2026-11-16T00:00:00.000Z";
        auto json = review_policy::toJson(record);
        const auto restored = review_policy::fromJson(json);
        QVERIFY(restored);
        QCOMPARE(review_policy::toJson(*restored), json);
        QVERIFY(!review_policy::blocks(*restored));
        closed.owner.clear();
        QVERIFY(!review_policy::fromJson(review_policy::toJson(record)));
        record.legacy = true;
        QVERIFY(review_policy::blocks(record));
        QVERIFY(!review_policy::fromJson(QJsonObject{}));
    }
};
QTEST_GUILESS_MAIN(TeamReviewPolicyTest)
#include "TeamReviewPolicyTest.moc"
