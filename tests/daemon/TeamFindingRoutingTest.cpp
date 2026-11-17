#include "AgentTestFixture.h"
#include "teams/TeamRoutingEvidence.h"

#include <QCryptographicHash>
#include <QEventLoop>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>

using namespace agent_test;
namespace teams = ketplus::daemon::teams;

namespace {
const QByteArray paragraph =
    "These existing words describe a harmless prose paragraph with enough ordinary language "
    "to demonstrate a verified line width correction while preserving every word and "
    "punctuation mark in the original paragraph.";
QByteArray wrapped(const int width) {
    QByteArray result, line;
    for (const auto& word : paragraph.split(' ')) {
        if (!line.isEmpty() && line.size() + word.size() + 1 > width) {
            result += line + '\n';
            line.clear();
        }
        if (!line.isEmpty())
            line += ' ';
        line += word;
    }
    return result + line + '\n';
}
bool git(const QString& directory, const QStringList& arguments, QByteArray* output = nullptr) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", arguments);
    if (!process.waitForFinished(10000) || process.exitCode() != 0)
        return false;
    if (output)
        *output = process.readAllStandardOutput();
    return true;
}
bool write(const QString& directory, const QString& path, const QByteArray& bytes) {
    const QString full = directory + '/' + path;
    if (!QDir().mkpath(QFileInfo(full).absolutePath()))
        return false;
    QFile file(full);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QString commit(const QString& directory) {
    QByteArray head;
    if (!git(directory, {"add", "-A"}) ||
        !git(directory,
             {"-c", "user.email=t@example.com", "-c", "user.name=T", "commit", "-qm", "fixture"}) ||
        !git(directory, {"rev-parse", "HEAD"}, &head))
        return {};
    return QString::fromLatin1(head.trimmed());
}
QString repository(const QString& directory) {
    QDir().mkpath(directory);
    if (!git(directory, {"init", "-qb", "integration"}) ||
        !write(directory, "docs/prose.md", wrapped(45)) ||
        !write(directory, "src/auth.cpp", "bool authorize() { return false; }\n"))
        return {};
    return commit(directory);
}
teams::review_policy::Record record(const QString& reviewed, const QString& reviewer) {
    teams::review_policy::Finding finding;
    finding.id = "f1";
    finding.severity = "p2";
    finding.impact = "behavior";
    finding.scope = "task";
    finding.mandatoryRule = false;
    finding.closeWhen = "prose_line_width_80";
    finding.evidence = QString::fromLatin1(
        QCryptographicHash::hash(wrapped(500), QCryptographicHash::Sha256).toHex());
    return {reviewed, reviewer, 1, false, {finding}};
}
QString reply(const QString& reviewer, const QString& reviewed, const bool defer = false) {
    auto reviewedRecord = record(reviewed, reviewer);
    if (defer) {
        reviewedRecord.findings[0].scope = "outside_task";
        reviewedRecord.findings[0].impact = "preference";
        reviewedRecord.findings[0].closeWhen = "owner_accepts_outside_preference";
    }
    auto json = teams::review_policy::toJson(reviewedRecord);
    auto finding = json.value("findings").toArray().first().toObject();
    for (const QString& key :
         {"status", "actor", "reason", "owner", "resolved_revision", "receipt", "at"})
        finding.remove(key);
    return "```ketplus-findings-v1\n" +
           QString::fromUtf8(
               QJsonDocument(QJsonObject{{"version", 1}, {"findings", QJsonArray{finding}}})
                   .toJson(QJsonDocument::Compact)) +
           "\n```\nVerdict: findings";
}
QJsonObject deferred(Fixture& fixture, const QString& session, const QJsonObject& payload) {
    QEventLoop loop;
    QJsonObject result;
    bool finished = false;
    fixture.host->handleAgentCommandDeferred(fixture.host->agentToken(session),
                                             "agents.team_task_route_verify", payload,
                                             [&](const QJsonObject& answer) {
                                                 result = answer;
                                                 finished = true;
                                                 loop.quit();
                                             });
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(20000);
    if (!finished)
        loop.exec();
    return result;
}
} // namespace

class TeamFindingRoutingTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void realThreeRevisionProofAndStoreFailClosed() {
        QTemporaryDir directory;
        const QString base = repository(directory.path());
        QVERIFY(!base.isEmpty());
        QVERIFY(write(directory.path(), "docs/prose.md", wrapped(500)));
        const QString reviewed = commit(directory.path());
        QVERIFY(!reviewed.isEmpty());
        QVERIFY(write(directory.path(), "docs/prose.md", wrapped(70)));
        const QString head = commit(directory.path());
        teams::Team scope;
        scope.id = 42;
        scope.revision = 7;
        scope.planApproved = true;
        scope.repository = directory.path();
        scope.integrationBranch = "integration";
        scope.members = {{.sessionId = "reviewer", .role = teams::role::reviewer}};
        teams::Task task;
        task.id = "T-1";
        task.changedRevision = 7;
        task.state = teams::state::inProgress;
        task.worktree = directory.path();
        task.branch = "integration";
        task.baseCommit = base;
        task.headCommit = reviewed;
        task.round = 1;
        task.reviewFindings = record(reviewed, "reviewer");
        const auto proof = teams::RoutingEvidence::produce(scope, task, head);
        QVERIFY2(proof.evidence, qPrintable(proof.reason));
        QCOMPARE(proof.recipe, "prose_line_width_fix_v1");
        QVERIFY(proof.evidence->localFixVerified);
        QCOMPARE(proof.evidence->reviewedRevision, reviewed);
        QCOMPARE(proof.evidence->revision, head);
        QCOMPARE(proof.evidence->checks.size(), 6);
        teams::RoutingEvidence receipts;
        receipts.retain(scope, task, *proof.evidence);
        QVERIFY(receipts.lookup(scope, task, proof.evidence->id));
        ++task.changedRevision;
        QVERIFY(!receipts.lookup(scope, task, proof.evidence->id));
        --task.changedRevision;
        for (const QString& severity : {"p0", "p1", "unknown"}) {
            task.reviewFindings->findings[0].severity = severity;
            QVERIFY(!teams::RoutingEvidence::produce(scope, task, head).evidence);
        }
        task.reviewFindings->findings[0].severity = "p2";
        task.reviewFindings->findings[0].mandatoryRule = true;
        QVERIFY(!teams::RoutingEvidence::produce(scope, task, head).evidence);
        task.reviewFindings->findings[0].mandatoryRule = false;
        task.reviewFindings->findings[0].impact = "required_check";
        QVERIFY(!teams::RoutingEvidence::produce(scope, task, head).evidence);
        task.reviewFindings->findings[0].impact = "behavior";
        task.reviewFindings->findings[0].closeWhen = "regression_passes";
        QVERIFY(!teams::RoutingEvidence::produce(scope, task, head).evidence);
        task.reviewFindings = record(reviewed, "reviewer");
        task.reviewFindings->revision = base;
        QVERIFY(!teams::RoutingEvidence::produce(scope, task, head).evidence);
        task.reviewFindings = record(reviewed, "reviewer");
        QVERIFY(write(directory.path(), "docs/prose.md", wrapped(500)));
        const QString unfixed = commit(directory.path());
        QVERIFY(!teams::RoutingEvidence::produce(scope, task, unfixed).evidence);
        QVERIFY(write(directory.path(), "docs/prose.md", "Different semantic content.\n"));
        const QString changed = commit(directory.path());
        QVERIFY(!teams::RoutingEvidence::produce(scope, task, changed).evidence);
        // A material change in the reviewed baseline cannot be hidden by reverting it
        // in the fix, even if the final base-to-head delta is only prose reflow.
        QVERIFY(write(directory.path(), "docs/prose.md", wrapped(500)));
        QVERIFY(write(directory.path(), "src/auth.cpp", "bool authorize() { return true; }\n"));
        const QString materialReviewed = commit(directory.path());
        task.headCommit = materialReviewed;
        task.reviewFindings = record(materialReviewed, "reviewer");
        QVERIFY(write(directory.path(), "docs/prose.md", wrapped(70)));
        QVERIFY(write(directory.path(), "src/auth.cpp", "bool authorize() { return false; }\n"));
        const QString reverted = commit(directory.path());
        QVERIFY(!teams::RoutingEvidence::produce(scope, task, reverted).evidence);
    }

    void authenticatedClosureUsesRealProducerStoreAndReviewedRecord_data() {
        QTest::addColumn<QString>("attack");
        QTest::newRow("positive") << QString{};
        QTest::newRow("deferred-preference") << QString("deferred-preference");
        QTest::newRow("ignored-gitlink") << QString("ignored-gitlink");
        QTest::newRow("replacement-tree") << QString("replacement-tree");
    }
    void authenticatedClosureUsesRealProducerStoreAndReviewedRecord() {
        QFETCH(QString, attack);
        const bool deferring = attack == "deferred-preference";
        Fixture fixture;
        QVERIFY(fixture.open(true));
        const QString repo = fixture.directory.filePath("repo");
        QVERIFY(!repository(repo).isEmpty());
        const auto command = [&](const QString& operation, const QJsonObject& payload) {
            return fixture.host->handleCommand(request(operation, payload), "desktop-test");
        };
        const auto made = command(
            "team.create",
            {{"repository", repo},
             {"name", "Prose closure"},
             {"integration_branch", "integration"},
             {"execution_policy", "full"},
             {"risk_acknowledged", true},
             {"plan", QJsonObject{{"leader", QJsonObject{{"provider", "claude"}}},
                                  {"developers",
                                   QJsonObject{{"count", 1},
                                               {"provider", "codex"},
                                               {"members", QJsonArray{QJsonObject{
                                                               {"paths", QJsonArray{"docs"}}}}}}},
                                  {"reviewer", QJsonObject{{"provider", "codex"}}}}}});
        QVERIFY2(made.value("ok").toBool(), qPrintable(text(made)));
        const QString teamId = made.value("team").toObject().value("id").toString();
        const auto members = made.value("team").toObject().value("members").toArray();
        const QString leader = members[0].toObject().value("session_id").toString();
        const QString developer = members[1].toObject().value("session_id").toString();
        const QString reviewer = members[2].toObject().value("session_id").toString();
        QVERIFY(
            command("team.update", {{"team_id", teamId}, {"paused", true}}).value("ok").toBool());
        const auto get = [&] {
            return fixture.agents(leader, "agents.team_get").value("team").toObject();
        };
        const auto task = [&] {
            return fixture.agents(leader, "agents.team_tasks", {{"task_id", "T-1"}})
                .value("task")
                .toObject();
        };
        QVERIFY(fixture
                    .agents(leader, "agents.team_task_create",
                            {{"title", "Wrap prose"}, {"criteria", QJsonArray{"Same prose words"}}})
                    .value("ok")
                    .toBool());
        QVERIFY(fixture
                    .agents(leader, "agents.team_task_assign",
                            {{"task_id", "T-1"}, {"owner", fixture.number(developer)}})
                    .value("ok")
                    .toBool());
        const auto decisions = command("team.get", {{"team_id", teamId}})
                                   .value("team")
                                   .toObject()
                                   .value("decisions")
                                   .toArray();
        QVERIFY(!decisions.isEmpty());
        const auto plan = decisions.first().toObject();
        QVERIFY(
            command("team.decision.answer", {{"team_id", teamId},
                                             {"decision_id", plan.value("id")},
                                             {"choice", plan.value("choices").toArray().first()}})
                .value("ok")
                .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!task().value("worktree").toString().isEmpty(), 10000);
        const QString worktree = task().value("worktree").toString();
        QVERIFY(fixture
                    .agents(developer, "agents.team_task_update",
                            {{"task_id", "T-1"},
                             {"state", "in_progress"},
                             {"met", QJsonArray{QJsonObject{{"index", 0}, {"met", true}}}}})
                    .value("ok")
                    .toBool());
        QVERIFY(write(worktree, "docs/prose.md", wrapped(500)));
        const QString reviewed = commit(worktree);
        QVERIFY(!reviewed.isEmpty());
        QVERIFY(write(fixture.directory.path(), QFileInfo(fixture.codexLog).fileName() + ".reply",
                      reply(reviewer, reviewed, deferring).toUtf8()));
        QVERIFY(fixture
                    .agents(developer, "agents.team_task_update",
                            {{"task_id", "T-1"}, {"state", "in_review"}})
                    .value("ok")
                    .toBool());
        QVERIFY(
            command("team.update", {{"team_id", teamId}, {"paused", false}}).value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(task().value("state").toString(), QString("in_progress"), 15000);
        QVERIFY(!task().value("review_findings").toObject().value("legacy").toBool(true));
        QCOMPARE(task().value("review_findings").toObject().value("revision").toString(), reviewed);
        QVERIFY(
            command("team.update", {{"team_id", teamId}, {"paused", true}}).value("ok").toBool());
        QString head = reviewed;
        if (!deferring) {
            QVERIFY(write(worktree, "docs/prose.md", wrapped(70)));
            head = commit(worktree);
        }
        if (attack == "ignored-gitlink") {
            QVERIFY(git(worktree, {"config", "diff.ignoreSubmodules", "all"}));
            QVERIFY(QDir().mkpath(worktree + "/docs/hidden.md"));
            QVERIFY(git(worktree, {"update-index", "--add", "--cacheinfo", "160000", head,
                                   "docs/hidden.md"}));
            head = commit(worktree);
        } else if (attack == "replacement-tree") {
            QVERIFY(write(worktree, "src/auth.cpp", "bool authorize() { return true; }\n"));
            head = commit(worktree);
            QByteArray tree, replacement;
            const QString base = task().value("base_commit").toString();
            QVERIFY(git(worktree, {"rev-parse", base + "^{tree}"}, &tree));
            QVERIFY(write(worktree, "docs/prose.md", wrapped(45)));
            QVERIFY(git(worktree, {"add", "docs/prose.md"}));
            QVERIFY(git(worktree, {"write-tree"}, &replacement));
            QVERIFY(git(worktree, {"reset", "--hard", head}));
            QVERIFY(git(worktree, {"replace", QString::fromLatin1(tree.trimmed()),
                                   QString::fromLatin1(replacement.trimmed())}));
        }
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.host->teamChecksPending(), 10000);
        const auto idle = [&] {
            for (const auto& m : get().value("members").toArray())
                if (!m.toObject().value("idle").toBool())
                    return false;
            return true;
        };
        QTRY_VERIFY_WITH_TIMEOUT(idle(), 10000);
        const auto evidence = deferred(fixture, developer,
                                       {{"task_id", "T-1"},
                                        {"expected_team_revision", get().value("revision")},
                                        {"head_commit", head}});
        if (!attack.isEmpty() && !deferring) {
            QVERIFY2(!evidence.value("ok").toBool(), qPrintable(text(evidence)));
            QCOMPARE(evidence.value("error_code").toString(), "review_required");
            QCOMPARE(task().value("state").toString(), "in_progress");
            return;
        }
        QVERIFY2(evidence.value("ok").toBool(), qPrintable(text(evidence)));
        QCOMPARE(evidence.value("recipe").toString(),
                 deferring ? "prose_preference_deferral_v1" : "prose_line_width_fix_v1");
        QJsonObject decision{
            {"version", 1},
            {"action", "resolve_findings"},
            {"confidence", 99},
            {"expected_team_revision", get().value("revision")},
            {"expected_task_revision", task().value("changed_revision")},
            {"head_commit", head},
            {"evidence_id", evidence.value("evidence_id")},
            {"resolutions",
             QJsonArray{QJsonObject{
                 {"id", "f1"}, {"status", "fixed"}, {"reason", "line_width_verified"}}}}};
        if (deferring)
            decision["resolutions"] = QJsonArray{QJsonObject{{"id", "f1"},
                                                             {"status", "deferred"},
                                                             {"reason", "outside_preference"},
                                                             {"owner", "followup_owner"}}};
        const auto payload = [&](const QJsonObject& d) {
            return QJsonObject{
                {"task_id", "T-1"},
                {"evidence", QString::fromUtf8(QJsonDocument(d).toJson(QJsonDocument::Compact))},
                {"reason", "bounded_fix_verified"}};
        };
        QCOMPARE(fixture.agents(developer, "agents.team_review_accept", payload(decision))
                     .value("error_code")
                     .toString(),
                 "role_forbidden");
        auto invented = decision;
        invented["evidence_id"] = QString(64, 'f');
        QCOMPARE(fixture.agents(leader, "agents.team_review_accept", payload(invented))
                     .value("error_code")
                     .toString(),
                 "evidence_missing");
        const auto accepted =
            fixture.agents(leader, "agents.team_review_accept", payload(decision));
        QVERIFY2(accepted.value("ok").toBool(), qPrintable(text(accepted)));
        QCOMPARE(task().value("state").toString(), "ready");
        QCOMPARE(task().value("head_commit").toString(), head);
        const auto closed = task()
                                .value("review_findings")
                                .toObject()
                                .value("findings")
                                .toArray()
                                .first()
                                .toObject();
        QCOMPARE(closed.value("status").toString(), deferring ? "deferred" : "fixed");
        if (deferring)
            QCOMPARE(closed.value("owner").toString(), "followup_owner");
        QCOMPARE(closed.value("actor").toString(), leader);
        QVERIFY(!fixture.agents(leader, "agents.team_review_accept", payload(decision))
                     .value("ok")
                     .toBool());
    }
};
QTEST_GUILESS_MAIN(TeamFindingRoutingTest)
#include "TeamFindingRoutingTest.moc"
