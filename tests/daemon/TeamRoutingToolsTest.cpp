#include "AgentTestFixture.h"
#include "agents/AgentTeamTools.h"
#include "teams/TeamAgentTasks.h"
#include "teams/TeamRoutingEvidence.h"

#include <QEventLoop>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>

using namespace agent_test;
namespace teams = ketplus::daemon::teams;

namespace {
const QByteArray original = "This paragraph contains the same words and meaning.\n";
const QByteArray reflow = "This paragraph contains\nthe same words and meaning.\n";

bool git(const QString& directory, const QStringList& args, QByteArray* output = nullptr) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", args);
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
QString init(const QString& directory, const QString& path = "docs/prose.md",
             const QByteArray& bytes = original) {
    QDir().mkpath(directory);
    if (!git(directory, {"init", "-qb", "integration"}) || !write(directory, path, bytes))
        return {};
    return commit(directory);
}
// Construct real committed deltas that ordinary Git commands can be configured
// to conceal. The host must inspect the actual objects, not that filtered view.
QString concealedDelta(const QString& directory, const QString& base, const QString& attack) {
    if (attack == "ignored-gitlink") {
        if (!git(directory, {"config", "diff.ignoreSubmodules", "all"}) ||
            !QDir().mkpath(directory + "/docs/hidden.md") ||
            !git(directory,
                 {"update-index", "--add", "--cacheinfo", "160000", base, "docs/hidden.md"}))
            return {};
        return commit(directory);
    }
    if (!write(directory, "src/auth.cpp", "bool authorize() { return true; }\n"))
        return {};
    const QString head = commit(directory);
    QByteArray baseTree, replacementTree;
    if (head.isEmpty() || !git(directory, {"rev-parse", base + "^{tree}"}, &baseTree) ||
        !write(directory, "docs/prose.md", original) || !git(directory, {"add", "docs/prose.md"}) ||
        !git(directory, {"write-tree"}, &replacementTree) ||
        !git(directory, {"reset", "--hard", head}) ||
        !git(directory, {"replace", QString::fromLatin1(baseTree.trimmed()),
                         QString::fromLatin1(replacementTree.trimmed())}))
        return {};
    return head;
}
teams::Task task(const QString& directory, const QString& base) {
    teams::Task result;
    result.id = "T-1";
    result.worktree = directory;
    result.branch = "integration";
    result.baseCommit = base;
    result.state = teams::state::inProgress;
    result.changedRevision = 7;
    return result;
}
teams::Team team() {
    teams::Team result;
    result.id = 42;
    result.revision = 7;
    result.integrationBranch = "integration";
    result.planApproved = true;
    return result;
}
QJsonObject deferred(Fixture& fixture, const QString& session, const QString& operation,
                     const QJsonObject& payload) {
    QEventLoop loop;
    QJsonObject result;
    bool finished = false;
    fixture.host->handleAgentCommandDeferred(fixture.host->agentToken(session), operation, payload,
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

class TeamRoutingToolsTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void realProducerChecksObjectsAndReceiptsExpireWithState_data() {
        QTest::addColumn<QByteArray>("before");
        QTest::addColumn<QByteArray>("after");
        QTest::newRow("soft-wrap") << original << reflow;
        QTest::newRow("ordinary-spaces")
            << original << QByteArray("This paragraph contains  the same words and meaning.\n");
        QTest::newRow("unicode-prose") << QByteArray("Các từ này giữ nguyên ý nghĩa.\n")
                                       << QByteArray("Các từ này\ngiữ nguyên ý nghĩa.\n");
    }
    void realProducerChecksObjectsAndReceiptsExpireWithState() {
        QFETCH(QByteArray, before);
        QFETCH(QByteArray, after);
        QTemporaryDir directory;
        const QString base = init(directory.path(), "docs/prose.md", before);
        QVERIFY(!base.isEmpty());
        QVERIFY(write(directory.path(), "docs/prose.md", after));
        const QString head = commit(directory.path());
        QVERIFY(!head.isEmpty());
        auto scope = team();
        auto work = task(directory.path(), base);
        const auto produced = teams::RoutingEvidence::produce(scope, work, head);
        QVERIFY2(produced.evidence.has_value(), qPrintable(produced.reason));
        QCOMPARE(teams::risk::evaluate(&*produced.evidence, 99).route,
                 teams::risk::Route::DeveloperLeader);
        QCOMPARE(produced.evidence->checks.size(), 4);
        QVERIFY(!produced.evidence->localFixVerified);
        QVERIFY(produced.evidence->reviewedRevision.isEmpty());
        teams::RoutingEvidence store;
        store.retain(scope, work, *produced.evidence);
        const QString id = produced.evidence->id;
        QVERIFY(store.lookup(scope, work, id).has_value());
        QVERIFY(!store.lookup(scope, work, QString(64, 'f')).has_value());
        QVERIFY(git(directory.path(), {"checkout", "-qb", "other"}));
        QVERIFY(!store.lookup(scope, work, id).has_value());
        QVERIFY(git(directory.path(), {"checkout", "-q", "integration"}));
        QVERIFY(write(directory.path(), "untracked", "dirty"));
        QVERIFY(!store.lookup(scope, work, id).has_value());
        QVERIFY(QFile::remove(directory.filePath("untracked")));
        ++work.changedRevision;
        QVERIFY(store.lookup(scope, work, id).has_value());
        --work.changedRevision;
        ++scope.revision;
        QVERIFY(store.lookup(scope, work, id).has_value());
        --scope.revision;
        work.baseCommit = head;
        QVERIFY(!store.lookup(scope, work, id).has_value());
    }

    void receiptSurvivesActivityButNotSemanticChanges() {
        QTemporaryDir directory;
        const QString base = init(directory.path());
        QVERIFY(write(directory.path(), "docs/prose.md", reflow));
        const QString head = commit(directory.path());
        auto scope = team();
        auto work = task(directory.path(), base);
        const auto proof = teams::RoutingEvidence::produce(scope, work, head);
        QVERIFY2(proof.evidence.has_value(), qPrintable(proof.reason));
        teams::RoutingEvidence store;
        store.retain(scope, work, *proof.evidence);
        const auto id = proof.evidence->id;

        // An unrelated task's note advances the document's optimistic fence only.
        ++scope.revision;
        QVERIFY(store.lookup(scope, work, id).has_value());
        // Reporting results on this task is also not a semantic edit.
        work.history.append({.kind = "note", .text = "Offline checks passed."});
        work.changedRevision = ++scope.revision;
        const auto refreshed = store.lookup(scope, work, id);
        QVERIFY(refreshed.has_value());
        QCOMPARE(refreshed->taskRevision, work.changedRevision);
        work.headCommit = head;
        QVERIFY(store.lookup(scope, work, id));

        const auto originalTask = work;
        work.headCommit = QString(40, 'a');
        QVERIFY(!store.lookup(scope, work, id));
        work = originalTask;
        scope.executionPolicy = "full";
        QVERIFY(!store.lookup(scope, work, id));
        scope.executionPolicy = "ask";
        scope.sharedPaths.append("src");
        QVERIFY(!store.lookup(scope, work, id));
        scope.sharedPaths.clear();
        work.title += " changed requirements";
        QVERIFY(!store.lookup(scope, work, id));
        work = originalTask;
        ++scope.settingsRevision;
        QVERIFY(!store.lookup(scope, work, id));
        --scope.settingsRevision;
        ++scope.membersRevision;
        QVERIFY(!store.lookup(scope, work, id));
        --scope.membersRevision;
        work.history.append({.kind = "verdict", .text = "findings"});
        QVERIFY(!store.lookup(scope, work, id));
    }

    void aTeamCannotExhaustOrEvictAnotherTeamsReceipts() {
        QTemporaryDir directory;
        const QString base = init(directory.path());
        QVERIFY(write(directory.path(), "docs/prose.md", reflow));
        const QString head = commit(directory.path());
        auto scope = team();
        const auto work = task(directory.path(), base);
        const auto proof = teams::RoutingEvidence::produce(scope, work, head);
        QVERIFY2(proof.evidence.has_value(), qPrintable(proof.reason));
        teams::RoutingEvidence store;
        QList<QString> ids;
        // Capacity coverage uses host-side copies of a real proof, never a
        // caller-supplied attestation. The authenticated boundary is tested below.
        for (int i = 0; i != 33; ++i) {
            auto evidence = *proof.evidence;
            evidence.id = QString::number(i, 16).rightJustified(64, '0');
            ids.append(evidence.id);
            store.retain(scope, work, evidence);
        }
        int retained = 0;
        for (const auto& id : ids)
            retained += store.lookup(scope, work, id).has_value();
        QCOMPARE(retained, 32);
        QVERIFY(!store.lookup(scope, work, ids.first()));
        QVERIFY(store.lookup(scope, work, ids.last()));

        const auto protectedTeam = scope;
        for (int teamId = 100; teamId != 104; ++teamId) {
            scope.id = teamId;
            for (int i = 0; i != 33; ++i) {
                auto evidence = *proof.evidence;
                evidence.teamId = teamId;
                evidence.id = QString::number(teamId * 100 + i, 16).rightJustified(64, '0');
                store.retain(scope, work, evidence);
            }
        }
        for (const auto& id : ids.sliced(1))
            QVERIFY(store.lookup(protectedTeam, work, id));
        QVERIFY(!store.lookup(scope, work, ids.last()));
    }

    void expiredReceiptsArePrunedBeforeLiveEvictionAndDuplicatesDoNotRenew() {
        QTemporaryDir directory;
        const QString base = init(directory.path());
        QVERIFY(write(directory.path(), "docs/prose.md", reflow));
        const QString head = commit(directory.path());
        auto scope = team();
        const auto work = task(directory.path(), base);
        const auto proof = teams::RoutingEvidence::produce(scope, work, head);
        QVERIFY2(proof.evidence.has_value(), qPrintable(proof.reason));
        qint64 now = 0;
        teams::RoutingEvidence store([&] { return now; });
        for (int i = 0; i != 127; ++i) {
            auto other = scope;
            other.id = 1000 + i;
            auto evidence = *proof.evidence;
            evidence.teamId = other.id;
            evidence.id = QString::number(1000 + i, 16).rightJustified(64, '0');
            QVERIFY(store.retain(other, work, evidence));
        }
        now = 1;
        QVERIFY(store.retain(scope, work, *proof.evidence));
        auto second = *proof.evidence;
        second.id = QString(64, 'e');
        auto foreign = scope;
        foreign.id = 9999;
        auto foreignProof = second;
        foreignProof.teamId = foreign.id;
        // Full store, no own slot: refuse admission without evicting another team.
        QVERIFY(!store.retain(foreign, work, foreignProof));
        QVERIFY(store.lookup(scope, work, proof.evidence->id));
        now = 600000;
        QVERIFY(!store.retain(scope, work, *proof.evidence));
        QVERIFY(store.retain(scope, work, second));
        // The expired foreign entries freed room, preserving this live receipt.
        QVERIFY(store.lookup(scope, work, proof.evidence->id));
        now = 600001;
        QVERIFY(!store.lookup(scope, work, proof.evidence->id));
        QVERIFY(store.lookup(scope, work, second.id));
        now = 1200000;
        QVERIFY(!store.lookup(scope, work, second.id));
    }

    void unknownSemanticCoverageFailsClosed_data() {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QByteArray>("before");
        QTest::addColumn<QByteArray>("after");
        QTest::newRow("changed-prose") << QString("docs/prose.md") << original
                                       << QByteArray("This paragraph now authorizes deletion.\n");
        QTest::newRow("same-words-different-paragraphs")
            << QString("docs/prose.md") << original
            << QByteArray("This paragraph contains\n\nthe same words and meaning.\n");
        QTest::newRow("hard-break")
            << QString("docs/prose.md") << original
            << QByteArray("This paragraph contains  \nthe same words and meaning.\n");
        QTest::newRow("markdown") << QString("docs/prose.md") << QByteArray("# A heading\n")
                                  << QByteArray("# A\nheading\n");
        QTest::newRow("executable-code") << QString("src/auth.cpp") << original << reflow;
        QTest::newRow("policy-data") << QString("docs/policy.json") << original << reflow;
    }
    void unknownSemanticCoverageFailsClosed() {
        QFETCH(QString, path);
        QFETCH(QByteArray, before);
        QFETCH(QByteArray, after);
        QTemporaryDir directory;
        const QString base = init(directory.path(), path, before);
        QVERIFY(!base.isEmpty());
        QVERIFY(write(directory.path(), path, after));
        const QString head = commit(directory.path());
        const auto result =
            teams::RoutingEvidence::produce(team(), task(directory.path(), base), head);
        QVERIFY(!result.evidence.has_value());
        QVERIFY(!result.reason.isEmpty());
    }

    void dirtyHeadModeAndPendingApprovalCannotMintReceipt() {
        QTemporaryDir directory;
        const QString base = init(directory.path());
        QVERIFY(write(directory.path(), "docs/prose.md", reflow));
        const QString head = commit(directory.path());
        auto scope = team();
        auto work = task(directory.path(), base);
        QVERIFY(!teams::RoutingEvidence::produce(scope, work, base).evidence);
        QVERIFY(write(directory.path(), "untracked", "dirty"));
        QCOMPARE(teams::RoutingEvidence::produce(scope, work, head).reason,
                 QString("stale_revision"));
        QVERIFY(QFile::remove(directory.filePath("untracked")));
        teams::Decision decision;
        decision.taskId = work.id;
        scope.decisions.append(decision);
        QVERIFY(!teams::RoutingEvidence::produce(scope, work, head).evidence);
        scope.decisions.clear();
        scope.planApproved = false;
        QVERIFY(!teams::RoutingEvidence::produce(scope, work, head).evidence);
        scope.planApproved = true;
        work.assignmentHeld = true;
        QVERIFY(!teams::RoutingEvidence::produce(scope, work, head).evidence);
        work.assignmentHeld = false;
        QVERIFY(git(directory.path(), {"config", "core.filemode", "false"}));
        QVERIFY(git(directory.path(), {"update-index", "--chmod=+x", "docs/prose.md"}));
        const QString modeHead = commit(directory.path());
        QVERIFY(!teams::RoutingEvidence::produce(scope, work, modeHead).evidence);
    }

    void authenticatedToolsUseRealProducerThenHostLookupThenPolicy_data() {
        QTest::addColumn<QString>("attack");
        QTest::newRow("ordinary-prose") << QString{};
        QTest::newRow("concurrent-only") << QString("concurrent-only");
        QTest::newRow("ignored-gitlink") << QString("ignored-gitlink");
        QTest::newRow("replaced-base-tree") << QString("replaced-base-tree");
    }
    void authenticatedToolsUseRealProducerThenHostLookupThenPolicy() {
        QFETCH(QString, attack);
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString repository = fixture.directory.filePath("repo");
        QVERIFY(!init(repository).isEmpty());
        QVERIFY(write(repository, "src/auth.cpp", "bool authorize() { return false; }\n"));
        QVERIFY(!commit(repository).isEmpty());
        const auto made = fixture.host->handleCommand(
            request("team.create",
                    {{"repository", repository},
                     {"name", "Prose"},
                     {"integration_branch", "integration"},
                     {"plan",
                      QJsonObject{{"leader", QJsonObject{{"provider", "claude"}}},
                                  {"developers",
                                   QJsonObject{{"count", 1},
                                               {"provider", "codex"},
                                               {"members", QJsonArray{QJsonObject{
                                                               {"paths", QJsonArray{"docs"}}}}}}},
                                  {"reviewer", QJsonObject{{"provider", "claude"}}}}}}),
            "desktop-test");
        QVERIFY2(made.value("ok").toBool(), qPrintable(text(made)));
        // Exercise the normal active team: delivery, notes and result reports
        // must not invalidate a proof whose semantic inputs remain unchanged.
        const QString teamId = made.value("team").toObject().value("id").toString();
        const auto members = made.value("team").toObject().value("members").toArray();
        QCOMPARE(members.size(), 3);
        const QString leader = members[0].toObject().value("session_id").toString();
        const QString developer = members[1].toObject().value("session_id").toString();
        const QString reviewer = members[2].toObject().value("session_id").toString();
        const auto revision = [&] {
            return fixture.agents(leader, "agents.team_get")
                .value("team")
                .toObject()
                .value("revision");
        };
        const auto readTask = [&] {
            return fixture.agents(leader, "agents.team_tasks", {{"task_id", "T-1"}})
                .value("task")
                .toObject();
        };
        // Advisory needs no task/worktree and cannot be replayed or impersonated.
        QJsonObject advice{{"expected_team_revision", revision()},
                           {"kind", "explanation"},
                           {"reason", "answer_question"}};
        QCOMPARE(fixture.agents(developer, "agents.team_task_route_decide", advice)
                     .value("error_code")
                     .toString(),
                 QString("role_forbidden"));
        const auto answered = fixture.agents(leader, "agents.team_task_route_decide", advice);
        QVERIFY2(answered.value("ok").toBool(), qPrintable(text(answered)));
        QCOMPARE(answered.value("routing_decision").toObject().value("route").toString(),
                 QString("leader_direct"));
        QVERIFY(fixture.agents(leader, "agents.team_tasks").value("tasks").toArray().isEmpty());
        QCOMPARE(fixture.agents(leader, "agents.team_task_route_decide", advice)
                     .value("error_code")
                     .toString(),
                 QString("stale_team"));
        auto created =
            fixture.agents(leader, "agents.team_task_create",
                           {{"title", "Reflow prose"}, {"criteria", QJsonArray{"Same words"}}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(text(created)));
        const auto assigned =
            fixture.agents(leader, "agents.team_task_assign",
                           {{"task_id", "T-1"}, {"owner", fixture.number(developer)}});
        QVERIFY2(assigned.value("ok").toBool(), qPrintable(text(assigned)));
        // The synthetic user approves the existing first-plan gate through the
        // normal client API. A routing tool cannot manufacture this approval.
        const auto planned =
            fixture.host->handleCommand(request("team.get", {{"team_id", teamId}}), "desktop-test");
        const auto decisions = planned.value("team").toObject().value("decisions").toArray();
        QVERIFY(!decisions.isEmpty());
        const auto plan = decisions.first().toObject();
        const auto approval = fixture.host->handleCommand(
            request("team.decision.answer", {{"team_id", teamId},
                                             {"decision_id", plan.value("id")},
                                             {"choice", plan.value("choices").toArray().first()}}),
            "desktop-test");
        QVERIFY2(approval.value("ok").toBool(), qPrintable(text(approval)));
        QTRY_VERIFY_WITH_TIMEOUT(!readTask().value("worktree").toString().isEmpty(), 10000);
        const QString worktree = readTask().value("worktree").toString();
        const auto updated =
            fixture.agents(developer, "agents.team_task_update",
                           {{"task_id", "T-1"},
                            {"state", "in_progress"},
                            {"met", QJsonArray{QJsonObject{{"index", 0}, {"met", true}}}}});
        QVERIFY2(updated.value("ok").toBool(), qPrintable(text(updated)));
        QVERIFY(write(worktree, "docs/prose.md", reflow));
        const QString head = commit(worktree);
        QVERIFY(!head.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.host->teamChecksPending(), 10000);
        const auto idle = [&] {
            const auto current = fixture.agents(leader, "agents.team_get").value("team").toObject();
            for (const auto& member : current.value("members").toArray())
                if (!member.toObject().value("idle").toBool())
                    return false;
            return true;
        };
        QTRY_VERIFY_WITH_TIMEOUT(idle(), 10000);
        QJsonObject verify{
            {"task_id", "T-1"}, {"expected_team_revision", revision()}, {"head_commit", head}};
        QJsonObject raced;
        bool raceFinished = false;
        fixture.host->handleAgentCommandDeferred(fixture.host->agentToken(developer),
                                                 "agents.team_task_route_verify", verify,
                                                 [&](const QJsonObject& result) {
                                                     raced = result;
                                                     raceFinished = true;
                                                 });
        if (attack != "concurrent-only") {
            const auto changedDuringCheck = fixture.agents(
                developer, "agents.team_task_update",
                {{"task_id", "T-1"}, {"note", "Synthetic state change while the host verifies."}});
            QVERIFY(changedDuringCheck.value("ok").toBool());
        }
        QTRY_VERIFY_WITH_TIMEOUT(raceFinished, 10000);
        QVERIFY2(raced.value("ok").toBool(), qPrintable(text(raced)));
        QVERIFY(raced.contains("evidence_id"));
        verify.insert("expected_team_revision", revision());
        auto forged = verify;
        forged.insert("checks", QJsonArray{QJsonObject{{"passed", true}}});
        QCOMPARE(deferred(fixture, developer, "agents.team_task_route_verify", forged)
                     .value("error_code")
                     .toString(),
                 QString("invalid_payload"));
        QCOMPARE(deferred(fixture, reviewer, "agents.team_task_route_verify", verify)
                     .value("error_code")
                     .toString(),
                 QString("role_forbidden"));
        // A semantic edit and a settings change while work is in flight must
        // reject even though ordinary team activity above is tolerated.
        for (const bool settings : {false, true}) {
            verify.insert("expected_team_revision", revision());
            bool finished = false;
            QJsonObject stale;
            fixture.host->handleAgentCommandDeferred(fixture.host->agentToken(developer),
                                                     "agents.team_task_route_verify", verify,
                                                     [&](const QJsonObject& answer) {
                                                         stale = answer;
                                                         finished = true;
                                                     });
            const auto changed =
                settings ? fixture.host->handleCommand(
                               request("team.update", {{"team_id", teamId}, {"paused", true}}),
                               "desktop-test")
                         : fixture.agents(
                               leader, "agents.team_task_criteria",
                               {{"task_id", "T-1"}, {"criteria", QJsonArray{"Updated words"}}});
            QVERIFY2(changed.value("ok").toBool(), qPrintable(text(changed)));
            QTRY_VERIFY_WITH_TIMEOUT(finished, 10000);
            QCOMPARE(stale.value("error_code").toString(), QString("stale_evidence"));
            QVERIFY(!stale.contains("evidence_id"));
            if (settings) {
                QVERIFY(fixture.host
                            ->handleCommand(
                                request("team.update", {{"team_id", teamId}, {"paused", false}}),
                                "desktop-test")
                            .value("ok")
                            .toBool());
            } else {
                QVERIFY(fixture
                            .agents(developer, "agents.team_task_update",
                                    {{"task_id", "T-1"},
                                     {"met", QJsonArray{QJsonObject{{"index", 0}, {"met", true}}}}})
                            .value("ok")
                            .toBool());
            }
        }
        verify.insert("expected_team_revision", revision());
        if (attack == "ignored-gitlink" || attack == "replaced-base-tree") {
            QByteArray base;
            QVERIFY(git(worktree, {"rev-parse", head + "^"}, &base));
            const QString baseCommit = QString::fromLatin1(base.trimmed());
            const QString concealedHead = concealedDelta(worktree, baseCommit, attack);
            QVERIFY(!concealedHead.isEmpty());
            QByteArray filtered;
            QVERIFY(git(worktree, {"diff", "--name-only", baseCommit, concealedHead}, &filtered));
            QCOMPARE(filtered.trimmed(), QByteArray("docs/prose.md"));
            QByteArray complete;
            QVERIFY(git(worktree,
                        {"--no-replace-objects", "diff", "--ignore-submodules=none", "--name-only",
                         baseCommit, concealedHead},
                        &complete));
            QVERIFY(
                complete.contains(attack == "ignored-gitlink" ? "docs/hidden.md" : "src/auth.cpp"));
            auto proofTask = task(worktree, baseCommit);
            proofTask.branch = readTask().value("branch").toString();
            const auto proof = teams::RoutingEvidence::produce(team(), proofTask, concealedHead);
            QVERIFY(!proof.evidence);
            QCOMPARE(proof.reason, attack == "ignored-gitlink"
                                       ? QString("unsupported_file_mode_or_content")
                                       : QString("unsupported_semantic_scope"));
            // The authenticated deferred tool runs the real producer. Neither
            // hidden material delta may mint a receipt or select the low-risk route.
            QTRY_VERIFY_WITH_TIMEOUT(!fixture.host->teamChecksPending(), 10000);
            verify.insert("expected_team_revision", revision());
            verify.insert("head_commit", concealedHead);
            const auto refused =
                deferred(fixture, developer, "agents.team_task_route_verify", verify);
            QCOMPARE(refused.value("error_code").toString(), QString("review_required"));
            QVERIFY(!refused.contains("evidence_id"));
            const auto denied = fixture.agents(leader, "agents.team_task_route_decide",
                                               {{"task_id", "T-1"},
                                                {"expected_team_revision", revision()},
                                                {"kind", "change"},
                                                {"route", "developer_leader"},
                                                {"head_commit", concealedHead},
                                                {"confidence", 99},
                                                {"reason", "material_delta_requires_review"}});
            QCOMPARE(denied.value("error_code").toString(), QString("review_required"));
            if (attack == "replaced-base-tree") {
                QByteArray tree;
                QVERIFY(git(worktree, {"--no-replace-objects", "rev-parse", baseCommit + "^{tree}"},
                            &tree));
                QVERIFY(git(worktree, {"replace", "-d", QString::fromLatin1(tree.trimmed())}));
            }
            QVERIFY(git(worktree, {"reset", "--hard", head}));
            verify.insert("head_commit", head);
        }
        verify.insert("expected_team_revision", revision());
        QList<QJsonObject> duplicateReplies;
        for (int i = 0; i != 2; ++i)
            fixture.host->handleAgentCommandDeferred(
                fixture.host->agentToken(developer), "agents.team_task_route_verify", verify,
                [&](const QJsonObject& answer) { duplicateReplies.append(answer); });
        QTRY_COMPARE_WITH_TIMEOUT(duplicateReplies.size(), 2, 20000);
        for (const auto& answer : duplicateReplies)
            QVERIFY2(answer.value("ok").toBool(), qPrintable(text(answer)));
        QVERIFY(duplicateReplies[0].value("evidence_id") !=
                duplicateReplies[1].value("evidence_id"));
        verify.insert("expected_team_revision", revision());
        const auto evidence = deferred(fixture, developer, "agents.team_task_route_verify", verify);
        QVERIFY2(evidence.value("ok").toBool(), qPrintable(text(evidence)));
        const QString id = evidence.value("evidence_id").toString();
        QVERIFY(teams::risk::evidenceId(id));
        QCOMPARE(evidence.value("recipe").toString(), QString("prose_reflow_v1"));
        if (attack != "concurrent-only") {
            const auto otherTask = fixture.agents(leader, "agents.team_task_create",
                                                  {{"title", "Unrelated active work"}});
            QVERIFY2(otherTask.value("ok").toBool(), qPrintable(text(otherTask)));
            const auto unrelatedNote = fixture.host->handleCommand(
                request("team.task.update",
                        {{"team_id", teamId}, {"task_id", "T-2"}, {"note", "Unrelated progress."}}),
                "desktop-test");
            QVERIFY2(unrelatedNote.value("ok").toBool(), qPrintable(text(unrelatedNote)));
            QVERIFY(fixture
                        .agents(developer, "agents.team_task_update",
                                {{"task_id", "T-1"}, {"note", "Verification results recorded."}})
                        .value("ok")
                        .toBool());
        }
        QJsonObject decide{{"task_id", "T-1"},    {"expected_team_revision", revision()},
                           {"kind", "change"},    {"route", "developer_leader"},
                           {"head_commit", head}, {"evidence_id", id},
                           {"confidence", 99},    {"reason", "verified_reflow"}};
        auto invented = decide;
        invented.insert("evidence_id", QString(64, 'f'));
        QCOMPARE(fixture.agents(leader, "agents.team_task_route_decide", invented)
                     .value("error_code")
                     .toString(),
                 QString("review_required"));
        invented = decide;
        invented.insert("actor", leader);
        QCOMPARE(fixture.agents(leader, "agents.team_task_route_decide", invented)
                     .value("error_code")
                     .toString(),
                 QString("invalid_payload"));
        const auto accepted = fixture.agents(leader, "agents.team_task_route_decide", decide);
        QVERIFY2(accepted.value("ok").toBool(), qPrintable(text(accepted)));
        QCOMPARE(accepted.value("routing_decision").toObject().value("route").toString(),
                 QString("developer_leader"));
        QCOMPARE(accepted.value("conclusion_source").toString(), QString("leader_policy"));
        QCOMPARE(accepted.value("reviewer_verdict").toBool(true), false);
        QCOMPARE(accepted.value("authorization_granted").toBool(true), false);
        QCOMPARE(readTask().value("state").toString(), QString("in_progress"));
        QCOMPARE(readTask().value("round").toInt(), 0);
        const auto row =
            fixture.agents(leader, "agents.team_tasks").value("tasks").toArray().first().toObject();
        QCOMPARE(row.value("routing_decision").toObject().value("actor").toString(), leader);
        QCOMPARE(row.value("state").toString(), QString("in_progress"));
        QCOMPARE(row.value("routing_status").toString(), QString("historical"));
        // Read the real recorded audit through the paired projection, without
        // adopting a leader identity or changing task/reviewer state.
        const teams::remote::Ids ids{
            [](const QString&) { return QStringLiteral("repo:fixture"); },
            [](const QString&) { return QStringLiteral("worktree:fixture"); }};
        QJsonObject phoneTask;
        bool phoneRead = false;
        fixture.host->remoteTeamCommand(
            request("team.task.get", {{"team_id", teamId}, {"task_id", "T-1"}}), ids,
            [&](const QJsonObject& result) {
                phoneTask = result;
                phoneRead = true;
            });
        QTRY_VERIFY_WITH_TIMEOUT(phoneRead, 10000);
        QVERIFY2(phoneTask.value("ok").toBool(), qPrintable(text(phoneTask)));
        const auto projected = phoneTask.value("task").toObject();
        QCOMPARE(projected.value("state").toString(), QString("in_progress"));
        QCOMPARE(projected.value("round").toInt(), 0);
        QCOMPARE(projected.value("routing_status").toString(), QString("historical"));
        const auto projectedDecision = projected.value("routing_decision").toObject();
        QCOMPARE(projectedDecision.value("actor").toString(), leader);
        QCOMPARE(projectedDecision.value("route").toString(), QString("developer_leader"));
        QCOMPARE(projectedDecision.value("revision").toString(), head);
        QVERIFY(projectedDecision.value("evidence_ids").toArray().contains(id));
        // The decision mutated revisions: its receipt is historical, not a reusable pass.
        decide.insert("expected_team_revision", revision());
        QCOMPARE(fixture.agents(leader, "agents.team_task_route_decide", decide)
                     .value("error_code")
                     .toString(),
                 QString("review_required"));
        // No receipt for a material semantic edit; confidence cannot compensate.
        QVERIFY(write(worktree, "docs/prose.md", "This now changes the approval rule.\n"));
        const QString changedHead = commit(worktree);
        verify.insert("expected_team_revision", revision());
        verify.insert("head_commit", changedHead);
        QCOMPARE(deferred(fixture, developer, "agents.team_task_route_verify", verify)
                     .value("error_code")
                     .toString(),
                 QString("review_required"));
        decide.remove("evidence_id");
        decide.insert("head_commit", changedHead);
        decide.insert("route", "auto");
        const auto conservative = fixture.agents(leader, "agents.team_task_route_decide", decide);
        QVERIFY2(conservative.value("ok").toBool(), qPrintable(text(conservative)));
        QCOMPARE(conservative.value("routing_decision").toObject().value("route").toString(),
                 QString("developer_reviewer"));
        QCOMPARE(readTask().value("state").toString(), QString("in_progress"));
        // Local UI cannot forge the leader's actor/session binding.
        auto clientPayload = decide;
        clientPayload.insert("actor_session", leader);
        QCOMPARE(
            fixture.host->handleCommand(request("team.route.decide", clientPayload), "desktop-test")
                .value("error_code")
                .toString(),
            QString("role_forbidden"));
        for (const QString& method : {QString("team.route.verify"), QString("team.route.decide")}) {
            QJsonObject phoneRefused;
            bool refused = false;
            fixture.host->remoteTeamCommand(request(method, clientPayload), ids,
                                            [&](const QJsonObject& result) {
                                                phoneRefused = result;
                                                refused = true;
                                            });
            QTRY_VERIFY_WITH_TIMEOUT(refused, 10000);
            QCOMPARE(phoneRefused.value("error_code").toString(), QString("role_forbidden"));
        }
    }

    void toolSchemasCannotSubmitEvidenceFacts() {
        bool verify = false, decide = false;
        for (const auto& item : agent_mcp::teamTools()) {
            const auto tool = item.toObject();
            if (tool.value("name") == "team_task_route_verify") {
                verify = true;
                const auto schema = tool.value("inputSchema").toObject();
                QCOMPARE(schema.value("additionalProperties").toBool(true), false);
                QCOMPARE(schema.value("properties").toObject().keys(),
                         QStringList({"expected_team_revision", "head_commit", "task_id"}));
            }
            decide = decide || tool.value("name") == "team_task_route_decide";
        }
        QVERIFY(verify && decide);
    }
};
QTEST_GUILESS_MAIN(TeamRoutingToolsTest)
#include "TeamRoutingToolsTest.moc"
