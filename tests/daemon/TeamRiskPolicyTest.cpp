#include "storage/LocalJournal.h"
#include "teams/TeamCompletionPolicy.h"
#include "teams/TeamCoordinator.h"

#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

using namespace ketplus::daemon;
using namespace ketplus::daemon::teams;

namespace {
const QString head(40, QLatin1Char('a'));
const QString receipt(64, QLatin1Char('b'));
risk::VerifiedEvidence verified() {
    risk::VerifiedEvidence e;
    e.id = receipt;
    e.revision = head;
    for (const QString& domain : risk::mandatoryDomains())
        e.domains[domain] = risk::Signal::Clear;
    e.requiredChecks = {QStringLiteral("focused_tests"), QStringLiteral("hygiene")};
    e.checks = {{"focused_tests", head, QString(64, 'c'), true},
                {"hygiene", head, QString(64, 'd'), true}};
    e.scopeEvidence = QString(64, 'e');
    e.rollbackEvidence = QString(64, 'f');
    e.materialRiskKnown = e.lowRiskVerified = e.userApprovalsSatisfied = true;
    e.unresolvedFindings = e.unresolvedP0 = e.unresolvedP1 = e.unresolvedFailures = 0;
    return e;
}
class Host final : public TeamHostPort {
  public:
    QHash<QString, TeamConversation> conversations;
    std::optional<risk::VerifiedEvidence> evidence;
    QString currentHead{head};
    bool dirty{false};
    bool unreadable{false};
    int allocations{0};
    QHash<QString, PullRequestFact> pullRequests;
    QString origin{completion::repositoryId("https://github.com/example/project")};
    QString completionRepository(const Team&, const Task&) const override { return origin; }
    void askPullRequests(const Team&, const QStringList&, PullRequestsDone done) override {
        done(pullRequests);
    }
    Host() {
        for (const QString& id :
             {QStringLiteral("lead"), QStringLiteral("dev"), QStringLiteral("rev")})
            conversations.insert(id, {.sessionId = id,
                                      .generation = 1,
                                      .number = id,
                                      .title = id,
                                      .provider = id == "rev" ? "claude" : "codex",
                                      .worktree = "/synthetic/repo",
                                      .idle = true});
    }
    std::optional<TeamConversation> teamConversation(const QString& id) const override {
        return conversations.contains(id) ? std::optional(conversations.value(id)) : std::nullopt;
    }
    QString conversationRepository(const QString&) const override { return "/synthetic/repo"; }
    std::optional<TaskWorktree> createTaskWorktree(const Team&, const Task&, QString*) override {
        ++allocations;
        return TaskWorktree{.branch = "fix/synthetic",
                            .worktree = "/synthetic/worktree",
                            .baseCommit = QString(40, '0')};
    }
    QString pointConversationAt(const QString&, const QString&) override { return {}; }
    QString postTeamMessage(const TeamMessage&) override { return {}; }
    QString taskCommit(const Team&, const Task&) const override { return currentHead; }
    std::optional<bool> hasUncommittedWork(const QString&) const override {
        return unreadable ? std::nullopt : std::optional(dirty);
    }
    std::optional<risk::VerifiedEvidence> routingEvidence(const Team&, const Task&,
                                                          const QString&) const override {
        return evidence;
    }
};
Team draft() {
    Team t;
    t.repository = "/synthetic/repo";
    t.name = "Routing";
    t.goal = "Verify routes";
    t.integrationBranch = "integration";
    t.planApproved = true;
    t.members = {{.sessionId = "lead", .role = role::leader, .name = "Leader"},
                 {.sessionId = "dev",
                  .role = role::developer,
                  .name = "Developer",
                  .lane = "Code",
                  .paths = {"src"}},
                 {.sessionId = "rev", .role = role::reviewer, .name = "Reviewer"}};
    return t;
}
} // namespace

class TeamRiskPolicyTest final : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> directory_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<Host> host_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};
    QString task_;
    QJsonObject request(const QString& route = "developer_leader") const {
        return {{"expected_team_revision", teams_->team(team_)->revision},
                {"kind", "change"},
                {"route", route},
                {"head_commit", head},
                {"evidence_id", receipt},
                {"confidence", 99},
                {"reason", "bounded_verified_change"}};
    }
    void refreshEvidence() {
        auto e = verified();
        e.teamId = team_;
        e.taskId = task_;
        e.taskRevision = teams_->team(team_)->task(task_)->changedRevision;
        host_->evidence = e;
    }
    QString error(const QJsonObject& r, const QString& actor = "lead") {
        return teams_->decideRoute(team_, task_, r, actor).error.value_or(TeamError{}).code;
    }
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void init() {
        directory_ = std::make_unique<QTemporaryDir>();
        journal_ = std::make_unique<LocalJournal>(directory_->filePath("journal.sqlite3"));
        host_ = std::make_unique<Host>();
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), host_.get());
        teams_->load();
        const auto made = teams_->create(draft(), "user");
        QVERIFY2(made.ok(), qPrintable(made.error.value_or(TeamError{}).message));
        team_ = made.teamId;
        QVERIFY(teams_
                    ->createTask(
                        team_,
                        {.title = "Synthetic change", .lane = "Code", .criteria = {"Verified"}},
                        "lead", &task_)
                    .ok());
        QVERIFY(teams_->assignTask(team_, task_, "dev", "lead").ok());
        QVERIFY(teams_->moveTask(team_, task_, state::inProgress, "dev").ok());
        QVERIFY(teams_
                    ->updateTask(team_, task_,
                                 {{"met", QJsonArray{QJsonObject{{"index", 0}, {"met", true}}}}},
                                 "dev")
                    .ok());
        refreshEvidence();
    }
    void cleanup() {
        teams_.reset();
        host_.reset();
        journal_.reset();
        directory_.reset();
    }
    void lowRiskPersistsWithoutVerdictOrCompletion() {
        const auto original = teams_->team(team_)->task(task_)->history.size();
        const auto replay = request();
        QVERIFY2(error(replay).isEmpty(), "Verified low risk must be accepted");
        const Task& t = *teams_->team(team_)->task(task_);
        QCOMPARE(t.state, state::ready);
        QVERIFY(t.routingDecision);
        QCOMPARE(t.routingDecision->route, risk::Route::DeveloperLeader);
        QCOMPARE(t.routingDecision->followUp, QStringLiteral("merge_then_acceptance"));
        QVERIFY(t.history.size() > original);
        QVERIFY(t.acceptance);
        QVERIFY(completion::valid(*teams_->team(team_), t));
        QCOMPARE(t.routingDecision->evidenceIds.size(), 3);
        QCOMPARE(error(replay), QStringLiteral("stale_team"));
        teams_.reset();
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), host_.get());
        teams_->load();
        const auto& restored = teams_->team(team_)->task(task_)->routingDecision;
        QVERIFY(restored);
        QCOMPARE(restored->revision, head);
        QCOMPARE(restored->actor, QStringLiteral("lead"));
        QCOMPARE(teams_->team(team_)->lastRoutingDecision->route, risk::Route::DeveloperLeader);
    }
    void verifiedRouteMustReachReadyAtTheProvenHead() {
        QVERIFY(error(request()).isEmpty());
        const Task& task = *teams_->team(team_)->task(task_);
        QCOMPARE(task.state, state::ready);
        QCOMPARE(task.headCommit, head);
        QCOMPARE(task.round, 0);
        QVERIFY(!task.reviewFindings);
    }
    void mandatoryReviewCannotBeClosedByAnInventedCommit() {
        host_->evidence.reset();
        QVERIFY(error(request("auto")).isEmpty());
        const auto closed = teams_->closeTask(team_, task_, QString(40, 'f'),
                                              "Caller says this was merged.", "lead");
        QVERIFY(closed.error);
        QCOMPARE(closed.error->code, QString("review_required"));
        QVERIFY(teams_->team(team_)->task(task_)->state != state::done);
    }
    void mandatoryReviewCannotBeBypassedByAMergedPullRequest() {
        const QString url = "https://github.com/example/project/pull/1";
        QVERIFY(teams_->updateTask(team_, task_, {{"pull_request", url}}, "dev").ok());
        host_->evidence.reset();
        QVERIFY(error(request("auto")).isEmpty());
        host_->pullRequests.insert(url, {.state = "merged", .mergeCommit = QString(40, 'e')});
        teams_->setPullRequestPeriod(60000, 0);
        teams_->checkMerged(team_);
        QVERIFY(teams_->team(team_)->task(task_)->state != state::done);
    }
    void verifiedPullRequestCompletesOnlyItsAcceptedHead_data() {
        QTest::addColumn<QString>("change");
        for (const QString& value : {"none", "pr_head", "branch", "base", "origin",
                                     "origin_changed", "live_head", "dirty", "missing_facts"})
            QTest::newRow(qPrintable(value)) << value;
    }
    void verifiedPullRequestCompletesOnlyItsAcceptedHead() {
        QFETCH(QString, change);
        const QString url = "https://github.com/example/project/pull/1";
        QVERIFY(teams_->updateTask(team_, task_, {{"pull_request", url}}, "dev").ok());
        refreshEvidence();
        QVERIFY(error(request()).isEmpty());
        QCOMPARE(teams_->team(team_)->task(task_)->state, state::ready);
        PullRequestFact fact{.state = "merged",
                             .mergeCommit = QString(40, 'e'),
                             .error = {},
                             .headCommit = head,
                             .headBranch = "fix/synthetic",
                             .baseBranch = "integration",
                             .url = url,
                             .repositoryId = host_->origin};
        if (change == "pr_head")
            fact.headCommit = QString(40, 'f');
        if (change == "branch")
            fact.headBranch = "other";
        if (change == "base")
            fact.baseBranch = "develop";
        if (change == "origin")
            fact.repositoryId = QString(64, 'a');
        if (change == "origin_changed")
            host_->origin = QString(64, 'a');
        if (change == "live_head")
            host_->currentHead = QString(40, 'f');
        if (change == "dirty")
            host_->dirty = true;
        if (change == "missing_facts")
            fact.headCommit.clear();
        host_->pullRequests.insert(url, fact);
        teams_->setPullRequestPeriod(60000, 0);
        teams_->checkMerged(team_);
        QCOMPARE(teams_->team(team_)->task(task_)->state == state::done, change == "none");
        if (change == "none") {
            const auto& history = teams_->team(team_)->task(task_)->history;
            QVERIFY(history.last().kind == "merged");
            QCOMPARE(history.last().data.value("accepted_head").toString(), head);
        }
    }
    void policyOrTaskChangesInvalidateAcceptance() {
        QVERIFY(error(request()).isEmpty());
        QVERIFY(teams_->updateTask(team_, task_, {{"note", "Unrelated result note"}}, "dev").ok());
        QCOMPARE(teams_->team(team_)->task(task_)->state, state::ready);
        QVERIFY(
            teams_->updateTask(team_, task_, {{"description", "New material scope"}}, "lead").ok());
        const Task& task = *teams_->team(team_)->task(task_);
        QCOMPARE(task.state, state::blocked);
        QVERIFY(!task.acceptance);
        QCOMPARE(task.reviewBlock, QString("acceptance_stale"));
        QVERIFY(teams_->closeTask(team_, task_, head, "Not a proof.", "lead").error);
    }
    void callerHashCannotCompleteEvenAnAcceptedTask() {
        QVERIFY(error(request()).isEmpty());
        QVERIFY(teams_->closeTask(team_, task_, QString(40, 'f'), "Claimed merge", "lead").error);
        QVERIFY(teams_->closeTask(team_, task_, head, "Request a real merge check", "lead").ok());
        QCOMPARE(teams_->team(team_)->task(task_)->state, state::ready);
    }
    void intentionalUserOverrideIsDistinctAndCannotBeUsedByAnAgent() {
        QVERIFY(teams_->moveTask(team_, task_, state::done, "dev", "Override", {}, true).error);
        QVERIFY(teams_->moveTask(team_, task_, state::done, "lead", "Override", {}, true).error);
        QVERIFY(teams_->moveTask(team_, task_, state::done, "user", {}, {}, true).error);
        QVERIFY(
            teams_->moveTask(team_, task_, state::done, "user", "Explicit user decision", {}, true)
                .ok());
        QCOMPARE(teams_->team(team_)->task(task_)->state, state::done);
        QCOMPARE(teams_->team(team_)->task(task_)->history.last().kind,
                 QString("user_completion_override"));
    }
    void acceptanceSerializationCannotGrantLegacyOrReboundAuthority() {
        QVERIFY(error(request()).isEmpty());
        const auto original = toJson(*teams_->team(team_));
        for (const QString& mutation : {"missing", "version", "head", "context", "actor", "team"}) {
            auto data = original;
            auto tasks = data.value("tasks").toArray();
            auto task = tasks[0].toObject();
            auto acceptance = task.value("completion_acceptance").toObject();
            if (mutation == "missing")
                task.remove("completion_acceptance");
            else {
                if (mutation == "version")
                    acceptance["version"] = 999;
                if (mutation == "head")
                    acceptance["head"] = QString(40, 'f');
                if (mutation == "context")
                    acceptance["context"] = QString(64, 'f');
                if (mutation == "actor")
                    acceptance["actor"] = "dev";
                task["completion_acceptance"] = acceptance;
            }
            tasks[0] = task;
            data["tasks"] = tasks;
            if (mutation == "team")
                data["id"] = QString::number(team_ + 1);
            const auto restored = teamFromJson(data);
            QVERIFY(!completion::valid(restored, *restored.task(task_)));
        }
        const auto restored = teamFromJson(original);
        QVERIFY(completion::valid(restored, *restored.task(task_)));
    }
    void repositoryIdentityIsOpaqueAndIgnoresCredentials() {
        const auto expected = completion::repositoryId("https://github.com/example/project");
        QVERIFY(risk::evidenceId(expected));
        QCOMPARE(expected, completion::repositoryId("git@github.com:example/project.git"));
        QCOMPARE(expected, completion::repositoryId(
                               "https://fixture:synthetic@github.com/example/project.git"));
        QCOMPARE(expected, completion::repositoryId("https://github.com/example/project/pull/1"));
        QVERIFY(expected != completion::repositoryId("https://github.com/example/fork"));
        QVERIFY(completion::repositoryId("/not/a/remote").isEmpty());
        QVERIFY(completion::repositoryId("https://github.com:8443/example/project").isEmpty());
        QVERIFY(
            completion::repositoryId("https://github.com/example/project?scope=other").isEmpty());
    }
    void adviceDoesNotAllocateATaskOrWorktree() {
        for (const QString& kind : {QStringLiteral("explanation"), QStringLiteral("proposal"),
                                    QStringLiteral("synthesis"), QStringLiteral("coordination")}) {
            const auto before = host_->allocations;
            const auto count = teams_->team(team_)->tasks.size();
            QVERIFY(teams_
                        ->decideRoute(team_, {},
                                      {{"expected_team_revision", teams_->team(team_)->revision},
                                       {"kind", kind},
                                       {"reason", "answer_request"}},
                                      "lead")
                        .ok());
            QCOMPARE(host_->allocations, before);
            QCOMPARE(teams_->team(team_)->tasks.size(), count);
            QCOMPARE(teams_->team(team_)->lastRoutingDecision->route, risk::Route::LeaderDirect);
        }
        QCOMPARE(teams_->team(team_)->task(task_)->state, state::inProgress);
    }
    void onlyTheSameTeamLeaderCanDecide() {
        for (const QString& actor : {QStringLiteral("dev"), QStringLiteral("rev"),
                                     QStringLiteral("user"), QStringLiteral("other_team_leader")})
            QCOMPARE(error(request(), actor), QStringLiteral("role_forbidden"));
    }
    void mandatoryAndUnknownDomains_data() {
        QTest::addColumn<QString>("domain");
        QTest::addColumn<bool>("unknown");
        for (const QString& domain : risk::mandatoryDomains()) {
            QTest::newRow(qPrintable(domain)) << domain << false;
            QTest::newRow(qPrintable(domain + "_unknown")) << domain << true;
        }
    }
    void mandatoryAndUnknownDomains() {
        QFETCH(QString, domain);
        QFETCH(bool, unknown);
        host_->evidence->domains[domain] =
            unknown ? risk::Signal::Unknown : risk::Signal::RequiresReview;
        QCOMPARE(error(request()), QStringLiteral("review_required"));
        QVERIFY(error(request("auto")).isEmpty());
        QCOMPARE(teams_->team(team_)->task(task_)->routingDecision->route,
                 risk::Route::DeveloperReviewer);
    }
    void missingOrBadEvidenceNeverFastPasses_data() {
        QTest::addColumn<int>("which");
        for (int i = 0; i < 17; ++i)
            QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void missingOrBadEvidenceNeverFastPasses() {
        QFETCH(int, which);
        auto& e = host_->evidence;
        switch (which) {
        case 0:
            e.reset();
            break;
        case 1:
            e->materialRiskKnown = false;
            break;
        case 2:
            e->lowRiskVerified = false;
            break;
        case 3:
            e->unresolvedP0 = 1;
            break;
        case 4:
            e->unresolvedP1 = 1;
            break;
        case 5:
            e->unresolvedFindings = 1;
            break;
        case 6:
            e->unresolvedFailures = 1;
            break;
        case 7:
            e->requiredChecks.clear();
            break;
        case 8:
            e->checks.removeLast();
            break;
        case 9:
            e->checks[0].passed = false;
            break;
        case 10:
            e->checks[0].revision = QString(40, 'b');
            break;
        case 11:
            e->checks[0].evidenceId.clear();
            break;
        case 12:
            e->rollbackEvidence.clear();
            break;
        case 13:
            e->scopeEvidence.clear();
            break;
        case 14:
            e->userApprovalsSatisfied = false;
            break;
        case 15:
            e->domains.remove("security");
            break;
        case 16:
            e->domains["unknown_future_domain"] = risk::Signal::Unknown;
            break;
        }
        QCOMPARE(error(request()), QStringLiteral("review_required"));
    }
    void revisionAndReceiptIdentityAreFenced() {
        host_->currentHead = QString(40, 'b');
        QCOMPARE(error(request()), QStringLiteral("stale_revision"));
        host_->currentHead = head;
        host_->evidence->revision = QString(40, 'b');
        QCOMPARE(error(request()), QStringLiteral("stale_evidence"));
        refreshEvidence();
        host_->evidence->teamId = team_ + 1;
        QCOMPARE(error(request()), QStringLiteral("stale_evidence"));
        refreshEvidence();
        host_->evidence->taskId = "T-99";
        QCOMPARE(error(request()), QStringLiteral("stale_evidence"));
        refreshEvidence();
        --host_->evidence->taskRevision;
        QCOMPARE(error(request()), QStringLiteral("stale_evidence"));
        refreshEvidence();
        host_->dirty = true;
        QCOMPARE(error(request()), QStringLiteral("stale_revision"));
        host_->dirty = false;
        host_->unreadable = true;
        QCOMPARE(error(request()), QStringLiteral("stale_revision"));
    }
    void confidenceAndPayloadCannotManufactureEvidence() {
        auto r = request();
        r["confidence"] = 90;
        QCOMPARE(error(r), QStringLiteral("review_required"));
        r = request();
        r["checks_passed"] = true;
        QCOMPARE(error(r), QStringLiteral("invalid_payload"));
        r = request();
        r.remove("expected_team_revision");
        QCOMPARE(error(r), QStringLiteral("stale_team"));
        r = request();
        r.remove("head_commit");
        QCOMPARE(error(r), QStringLiteral("stale_revision"));
        r = request();
        r.remove("reason");
        QCOMPARE(error(r), QStringLiteral("invalid_payload"));
        r = request();
        r["reason"] = "raw tool output / a token";
        QCOMPARE(error(r), QStringLiteral("invalid_payload"));
        r = request();
        r.remove("evidence_id");
        QCOMPARE(error(r), QStringLiteral("review_required"));
        r = request();
        r["confidence"] = 99.5;
        QCOMPARE(error(r), QStringLiteral("invalid_payload"));
        r = request();
        r["reason"] = "bounded_change\n";
        QCOMPARE(error(r), QStringLiteral("invalid_payload"));
        r = request();
        r["evidence_id"] = receipt + "\n";
        QCOMPARE(error(r), QStringLiteral("invalid_payload"));
        host_->evidence.reset();
        QCOMPARE(error(request()), QStringLiteral("review_required"));
    }
    void incompleteAcceptanceAndExistingReviewsStayWithReviewer() {
        QVERIFY(teams_
                    ->updateTask(team_, task_,
                                 {{"met", QJsonArray{QJsonObject{{"index", 0}, {"met", false}}}}},
                                 "dev")
                    .ok());
        refreshEvidence();
        QCOMPARE(error(request()), QStringLiteral("review_required"));
        // Persisted review history is an independent daemon gate, not receipt severity.
        auto stored = toJson(*teams_->team(team_));
        auto tasks = stored["tasks"].toArray();
        auto t = tasks[0].toObject();
        t["round"] = 1;
        t["criteria"] = QJsonArray{QJsonObject{{"text", "Verified"}, {"met", true}}};
        tasks[0] = t;
        stored["tasks"] = tasks;
        const auto revision = teams_->team(team_)->revision;
        stored["revision"] = revision + 1;
        QString why;
        QVERIFY(journal_->saveTeam(team_, revision, stored, teams_->team(team_)->memberSessions(),
                                   &why));
        teams_.reset();
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), host_.get());
        teams_->load();
        refreshEvidence();
        QCOMPARE(error(request()), QStringLiteral("review_locked"));
    }
    void contractFixtureIsSyntheticAndUsesTheCurrentDomainSet() {
        QFile file(QFINDTESTDATA("fixtures/team-risk-routing-v1.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        const auto data = doc.object();
        QCOMPARE(data.value("version").toInt(), 1);
        QCOMPARE(data.value("mandatory_domains").toArray(),
                 QJsonArray::fromStringList(risk::mandatoryDomains()));
        auto r = data.value("change_request").toObject();
        r["expected_team_revision"] = teams_->team(team_)->revision;
        QVERIFY(this->error(r).isEmpty());
        const auto record = risk::toJson(*teams_->team(team_)->task(task_)->routingDecision);
        const auto decoded = risk::fromJson(record);
        QVERIFY(decoded);
        QCOMPARE(risk::toJson(*decoded), record);
        QVERIFY(!host_->TeamHostPort::routingEvidence(*teams_->team(team_),
                                                      *teams_->team(team_)->task(task_), receipt));
    }
    void persistedDecisionCannotBeReboundToAnotherTeamOrTask() {
        QVERIFY(error(request()).isEmpty());
        auto data = toJson(*teams_->team(team_));
        data["id"] = QString::number(team_ + 1);
        const auto wrongTeam = teamFromJson(data);
        QVERIFY(!wrongTeam.lastRoutingDecision);
        QVERIFY(!wrongTeam.task(task_)->routingDecision);
        data = toJson(*teams_->team(team_));
        auto tasks = data.value("tasks").toArray();
        auto task = tasks[0].toObject();
        auto decision = task.value("routing_decision").toObject();
        decision["task_id"] = "T-99";
        task["routing_decision"] = decision;
        tasks[0] = task;
        data["tasks"] = tasks;
        QVERIFY(!teamFromJson(data).task(task_)->routingDecision);
    }
    void legacyAndMalformedSerializationCannotGrantFastPass() {
        auto t = toJson(*teams_->team(team_));
        t.remove("last_routing_decision");
        const auto legacy = teamFromJson(t);
        QVERIFY(!legacy.lastRoutingDecision);
        QVERIFY(!legacy.task(task_)->routingDecision);
        QVERIFY(!risk::fromJson(QJsonObject{{"version", 99}, {"route", "developer_leader"}}));
        auto empty = toJson(legacy);
        auto rows = empty["tasks"].toArray();
        auto row = rows[0].toObject();
        row["routing_decision"] = QJsonObject{{"version", 1}, {"route", "developer_leader"}};
        rows[0] = row;
        empty["tasks"] = rows;
        QVERIFY(!teamFromJson(empty).task(task_)->routingDecision);
        host_->evidence.reset();
        QCOMPARE(error(request()), QStringLiteral("review_required"));
    }
};
QTEST_GUILESS_MAIN(TeamRiskPolicyTest)
#include "TeamRiskPolicyTest.moc"
