// T-2: team.provider.set moves a team's members to another provider through the provider
// handoff, never interrupting a turn. Only the scripted fake providers run; no quota is spent.
#include "AgentTestFixture.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTest>

using namespace agent_test;

namespace {

QJsonObject plan() {
    return {{"leader", QJsonObject{{"provider", "claude"}, {"model", "opus"}}},
            {"developers",
             QJsonObject{{"count", 2},
                         {"provider", "codex"},
                         {"model", "gpt-5"},
                         {"effort", "medium"},
                         {"members", QJsonArray{QJsonObject{{"paths", QJsonArray{"apps/web"}}},
                                                QJsonObject{{"paths", QJsonArray{"apps/api"}}}}}}},
            {"reviewer", QJsonObject{{"provider", "claude"}, {"model", "sonnet"}}}};
}

} // namespace

class TeamProviderSwitchTest final : public QObject {
    Q_OBJECT

  private:
    QJsonObject command(Fixture& fixture, const QString& type, const QJsonObject& payload) {
        return fixture.host->handleCommand(request(type, payload), "desktop-test");
    }

    QJsonObject create(Fixture& fixture, const QString& policy = QStringLiteral("ask")) {
        return command(fixture, "team.create",
                       {{"repository", fixture.directory.path()},
                        {"name", "Checkout"},
                        {"integration_branch", "integration"},
                        {"execution_policy", policy},
                        {"risk_acknowledged", policy == "full"},
                        {"plan", plan()}});
    }

    static QString memberWith(const QJsonObject& team, const QString& name) {
        for (const QJsonValue& value : team.value("members").toArray())
            if (value.toObject().value("name").toString() == name)
                return value.toObject().value("session_id").toString();
        return {};
    }

    static QJsonObject memberOf(const QJsonObject& team, const QString& sessionId) {
        for (const QJsonValue& value : team.value("members").toArray())
            if (value.toObject().value("session_id").toString() == sessionId)
                return value.toObject();
        return {};
    }

    static QJsonObject resultFor(const QJsonObject& answer, const QString& sessionId) {
        for (const QJsonValue& value : answer.value("results").toArray())
            if (value.toObject().value("session_id").toString() == sessionId)
                return value.toObject();
        return {};
    }

    static StoredSession stored(Fixture& fixture, const QString& sessionId) {
        for (const StoredSession& session : fixture.journal->sessions())
            if (session.id == sessionId)
                return session;
        return {};
    }

    QJsonObject team(Fixture& fixture, const QJsonObject& created) {
        return command(fixture, "team.get", {{"team_id", created.value("id").toString()}})
            .value("team")
            .toObject();
    }

    QStringList updateKinds(Fixture& fixture, const QJsonObject& created) {
        QStringList kinds;
        for (const QJsonValue& value :
             command(fixture, "team.get", {{"team_id", created.value("id").toString()}})
                 .value("updates")
                 .toArray())
            kinds.append(value.toObject().value("kind").toString());
        return kinds;
    }

    // A turn of the member, as the desktop sends one: no provider in the options.
    bool turn(Fixture& fixture, const QString& sessionId, const QString& prompt,
              const QJsonObject& extra = {}) {
        const int before = fixture.count(sessionId, "turn.completed");
        QJsonObject payload = extra;
        payload.insert("prompt", prompt);
        const QJsonObject started = fixture.host->handleCommand(
            request("session.start_turn", payload, sessionId, 1), "desktop-test");
        if (!started.value("ok").toBool()) {
            qWarning("%s", qPrintable(text(started)));
            return false;
        }
        return QTest::qWaitFor([&] { return fixture.count(sessionId, "turn.completed") > before; },
                               10000);
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // Codex -> Claude for the whole team, then Claude -> Codex for one developer. Each next
    // turn runs on the new provider in a fresh native context, pointed at the transcript.
    void theWholeTeamMovesAndBack() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QJsonObject created = create(fixture).value("team").toObject();
        QVERIFY(!created.isEmpty());
        const QString developer = memberWith(created, "Developer 1");
        const QString other = memberWith(created, "Developer 2");
        const QString reviewer = memberWith(created, "Reviewer");
        QVERIFY(turn(fixture, developer, "First question"));
        QCOMPARE(stored(fixture, developer).adapter, QStringLiteral("codex"));

        const QJsonObject switched =
            command(fixture, "team.provider.set",
                    {{"team_id", created.value("id").toString()}, {"provider", "claude"}});
        QVERIFY2(switched.value("ok").toBool(), qPrintable(text(switched)));
        QCOMPARE(resultFor(switched, developer).value("status").toString(),
                 QStringLiteral("pending"));
        QCOMPARE(resultFor(switched, developer).value("applies").toString(),
                 QStringLiteral("next_turn"));
        QCOMPARE(resultFor(switched, reviewer).value("status").toString(),
                 QStringLiteral("unchanged"));
        const QJsonObject after = switched.value("team").toObject();
        QCOMPARE(after.value("provider").toString(), QStringLiteral("claude"));
        QCOMPARE(memberOf(after, developer).value("pending_switch").toObject().value("provider"),
                 QJsonValue("claude"));
        QVERIFY(!memberOf(after, reviewer).contains("pending_switch"));
        QVERIFY(updateKinds(fixture, created).contains("provider_set"));
        // Nothing moved yet, and the pending switch survives a restart.
        QCOMPARE(stored(fixture, developer).adapter, QStringLiteral("codex"));
        fixture.restart();
        QCOMPARE(memberOf(team(fixture, created), other)
                     .value("pending_switch")
                     .toObject()
                     .value("provider"),
                 QJsonValue("claude"));

        QVERIFY(turn(fixture, developer, "Carry on"));
        const StoredSession moved = stored(fixture, developer);
        QCOMPARE(moved.adapter, QStringLiteral("claude"));
        // The Codex model does not carry over; the new provider's default applies.
        QVERIFY(moved.model.isEmpty());
        QVERIFY(moved.effort.isEmpty());
        QCOMPARE(fixture.count(developer, "session.provider_changed"), 1);
        // The turn reports the model it left (gpt-5 -> Claude's default) as changed.
        const QList<QJsonObject> configured =
            fixture.payloads(developer, "session.configuration_changed");
        QVERIFY(!configured.isEmpty());
        QVERIFY(configured.last().value("changed").toArray().contains(QJsonValue("model")));
        QCOMPARE(configured.last().value("adapter").toString(), QStringLiteral("claude"));
        const QString transcript =
            QDir(fixture.directory.path())
                .filePath(QStringLiteral("handoff/%1/transcript.md").arg(developer));
        QVERIFY(QFileInfo::exists(transcript));
        QVERIFY(logText(transcript).contains("First question"));
        const QByteArray claudeSeen = logText(fixture.claudeLog);
        QVERIFY(claudeSeen.contains("[KetPlus handoff]"));
        QVERIFY(claudeSeen.contains(transcript.toUtf8()));
        QVERIFY(!memberOf(team(fixture, created), developer).contains("pending_switch"));
        QVERIFY(updateKinds(fixture, created).contains("provider_switched"));
        // A developer added later starts on the team's provider.
        const QJsonObject added = command(fixture, "team.member.add",
                                          {{"team_id", created.value("id").toString()},
                                           {"name", "Docs writer"},
                                           {"paths", QJsonArray{"docs"}}});
        QVERIFY2(added.value("ok").toBool(), qPrintable(text(added)));
        QCOMPARE(memberOf(added.value("team").toObject(),
                          memberWith(added.value("team").toObject(), "Docs writer"))
                     .value("provider")
                     .toString(),
                 QStringLiteral("claude"));

        // Back to Codex for one developer only, with a model of its own.
        const QJsonObject back = command(fixture, "team.provider.set",
                                         {{"team_id", created.value("id").toString()},
                                          {"provider", "codex"},
                                          {"model", "gpt-5"},
                                          {"members", QJsonArray{developer}}});
        QVERIFY2(back.value("ok").toBool(), qPrintable(text(back)));
        QCOMPARE(back.value("results").toArray().size(), 1);
        // A chosen part of the team leaves the team's provider as it was.
        QCOMPARE(back.value("team").toObject().value("provider").toString(),
                 QStringLiteral("claude"));
        QVERIFY(memberOf(back.value("team").toObject(), other).contains("pending_switch"));
        const qsizetype codexTurns = logText(fixture.codexLog).count("turn:");
        QVERIFY(turn(fixture, developer, "Back again"));
        QCOMPARE(stored(fixture, developer).adapter, QStringLiteral("codex"));
        QCOMPARE(stored(fixture, developer).model, QStringLiteral("gpt-5"));
        QCOMPARE(fixture.count(developer, "session.provider_changed"), 2);
        QCOMPARE(logText(fixture.codexLog).count("turn:"), codexTurns + 1);
    }

    // A member whose turn waits for an approval keeps it; its switch applies to the next one.
    void aRunningTurnIsNeverInterrupted() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QJsonObject created = create(fixture).value("team").toObject();
        const QString developer = memberWith(created, "Developer 1");
        QVERIFY(fixture.host
                    ->handleCommand(
                        request("session.start_turn", {{"prompt", "approve"}}, developer, 1),
                        "desktop-test")
                    .value("ok")
                    .toBool());
        QString approvalId;
        QVERIFY(QTest::qWaitFor(
            [&] {
                for (const QJsonObject& payload : fixture.payloads(developer, "approval.requested"))
                    approvalId = payload.value("approval_id").toString();
                return !approvalId.isEmpty();
            },
            10000));
        const QJsonObject switched =
            command(fixture, "team.provider.set",
                    {{"team_id", created.value("id").toString()}, {"provider", "claude"}});
        QVERIFY2(switched.value("ok").toBool(), qPrintable(text(switched)));
        QCOMPARE(resultFor(switched, developer).value("applies").toString(),
                 QStringLiteral("after_current_turn"));
        QCOMPARE(memberOf(switched.value("team").toObject(), developer).value("idle"),
                 QJsonValue(false));
        QCOMPARE(fixture.count(developer, "turn.interrupted"), 0);
        // A start_turn the busy member refuses leaves the switch pending.
        QCOMPARE(fixture.host
                     ->handleCommand(
                         request("session.start_turn", {{"prompt", "Too soon"}}, developer, 1),
                         "desktop-test")
                     .value("error_code")
                     .toString(),
                 QStringLiteral("session_busy"));
        QVERIFY(memberOf(team(fixture, created), developer).contains("pending_switch"));
        QCOMPARE(stored(fixture, developer).adapter, QStringLiteral("codex"));
        QVERIFY(fixture.host
                    ->handleCommand(request("approval.resolve",
                                            {{"approval_id", approvalId},
                                             {"resolution", QJsonObject{{"decision", "accept"}}}},
                                            developer, 1),
                                    "desktop-test")
                    .value("ok")
                    .toBool());
        QVERIFY(QTest::qWaitFor([&] { return fixture.count(developer, "turn.completed") == 1; },
                                10000));
        // The turn finished on Codex; the switch still waits for the next one.
        QCOMPARE(stored(fixture, developer).adapter, QStringLiteral("codex"));
        QVERIFY(memberOf(team(fixture, created), developer).contains("pending_switch"));
        QVERIFY(turn(fixture, developer, "Next"));
        QCOMPARE(stored(fixture, developer).adapter, QStringLiteral("claude"));
    }

    // A Codex reviewer needs the team's Full access; it is never granted without asking.
    void aCodexReviewerNeedsFullAccess() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QJsonObject created = create(fixture).value("team").toObject();
        const QString reviewer = memberWith(created, "Reviewer");
        const QJsonObject refused =
            command(fixture, "team.provider.set",
                    {{"team_id", created.value("id").toString()}, {"provider", "codex"}});
        QCOMPARE(refused.value("error_code").toString(), QStringLiteral("reviewer_unsupported"));
        QCOMPARE(refused.value("session_id").toString(), reviewer);
        const QJsonObject unchanged = team(fixture, created);
        QCOMPARE(unchanged.value("execution_policy").toString(), QStringLiteral("ask"));
        for (const QJsonValue& value : unchanged.value("members").toArray())
            QVERIFY(!value.toObject().contains("pending_switch"));

        // With Full access chosen for the team, the reviewer takes it as a Codex reviewer.
        Fixture full;
        QVERIFY(full.open());
        const QJsonObject trusted = create(full, "full").value("team").toObject();
        const QString fullReviewer = memberWith(trusted, "Reviewer");
        QCOMPARE(stored(full, fullReviewer).executionPolicy, QStringLiteral("ask"));
        const QJsonObject switched =
            command(full, "team.provider.set",
                    {{"team_id", trusted.value("id").toString()}, {"provider", "codex"}});
        QVERIFY2(switched.value("ok").toBool(), qPrintable(text(switched)));
        QVERIFY(turn(full, fullReviewer, "Ready?"));
        QCOMPARE(stored(full, fullReviewer).adapter, QStringLiteral("codex"));
        QCOMPARE(stored(full, fullReviewer).executionPolicy, QStringLiteral("full"));
    }

    // T-3: Full access comes with the switch only when the user acknowledged it, as for
    // team.create, and never when the switch is refused. Every member but a Claude reviewer
    // takes it at its next turn, also one whose agent does not change.
    void fullAccessComesWithTheSwitchWhenAcknowledged() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QJsonObject created = create(fixture).value("team").toObject();
        const QString teamId = created.value("id").toString();
        const QString leader = memberWith(created, "Lead Checkout");
        const QString developer = memberWith(created, "Developer 1");
        const QString reviewer = memberWith(created, "Reviewer");
        QCOMPARE(command(fixture, "team.provider.set",
                         {{"team_id", teamId}, {"provider", "codex"}, {"execution_policy", "full"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("execution_policy_denied"));
        QCOMPARE(command(fixture, "team.provider.set",
                         {{"team_id", teamId},
                          {"provider", "codex"},
                          {"execution_policy", "ask"},
                          {"risk_acknowledged", true}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("invalid_payload"));
        QCOMPARE(command(fixture, "team.provider.set",
                         {{"team_id", teamId},
                          {"provider", "copilot"},
                          {"execution_policy", "full"},
                          {"risk_acknowledged", true}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("provider_unavailable"));
        QCOMPARE(team(fixture, created).value("execution_policy").toString(),
                 QStringLiteral("ask"));
        QVERIFY(!updateKinds(fixture, created).contains("execution_policy_changed"));

        const QJsonObject switched = command(fixture, "team.provider.set",
                                             {{"team_id", teamId},
                                              {"provider", "codex"},
                                              {"execution_policy", "full"},
                                              {"risk_acknowledged", true}});
        QVERIFY2(switched.value("ok").toBool(), qPrintable(text(switched)));
        QCOMPARE(resultFor(switched, reviewer).value("status").toString(),
                 QStringLiteral("pending"));
        QCOMPARE(resultFor(switched, developer).value("status").toString(),
                 QStringLiteral("unchanged"));
        const QJsonObject now = team(fixture, created);
        QCOMPARE(now.value("execution_policy").toString(), QStringLiteral("full"));
        QCOMPARE(memberOf(now, leader).value("pending_policy").toString(), QStringLiteral("full"));
        QCOMPARE(memberOf(now, developer).value("pending_policy").toString(),
                 QStringLiteral("full"));
        QVERIFY(!memberOf(now, reviewer).contains("pending_policy"));
        const QStringList kinds = updateKinds(fixture, created);
        QVERIFY(kinds.contains("execution_policy_changed"));
        QVERIFY(kinds.contains("provider_set"));
        // Nothing changes before a turn: the policy waits with the member.
        QCOMPARE(stored(fixture, developer).executionPolicy, QStringLiteral("ask"));

        // A desktop composer sends the conversation's current permission with every prompt; the
        // team's Full access still reaches the member with that turn.
        const QJsonObject asked{{"options", QJsonObject{{"permission", "workspace-write"}}}};
        for (const QString& member : {leader, developer, reviewer})
            QVERIFY(turn(fixture, member, "Ready?", asked));
        QCOMPARE(stored(fixture, developer).executionPolicy, QStringLiteral("full"));
        QCOMPARE(stored(fixture, developer).adapter, QStringLiteral("codex"));
        QCOMPARE(stored(fixture, leader).executionPolicy, QStringLiteral("full"));
        QCOMPARE(stored(fixture, leader).adapter, QStringLiteral("codex"));
        QCOMPARE(stored(fixture, reviewer).executionPolicy, QStringLiteral("full"));
        QCOMPARE(stored(fixture, reviewer).adapter, QStringLiteral("codex"));
        // A member that has not taken a turn yet still waits for it.
        const QJsonObject after = team(fixture, created);
        for (const QString& member : {leader, developer})
            QVERIFY(!memberOf(after, member).contains("pending_policy"));
        QCOMPARE(
            memberOf(after, memberWith(created, "Developer 2")).value("pending_policy").toString(),
            QStringLiteral("full"));
    }

    // A provider that cannot take a turn refuses the change before anything is recorded.
    void aProviderThatIsNotReadyIsRefused() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QJsonObject created = create(fixture).value("team").toObject();
        const QJsonObject missing =
            command(fixture, "team.provider.set",
                    {{"team_id", created.value("id").toString()}, {"provider", "copilot"}});
        QCOMPARE(missing.value("error_code").toString(), QStringLiteral("provider_unavailable"));
        QVERIFY(!missing.value("error_message").toString().isEmpty());
        QCOMPARE(command(fixture, "team.provider.set",
                         {{"team_id", created.value("id").toString()}, {"provider", "gemini"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("invalid_payload"));
        QCOMPARE(command(fixture, "team.provider.set",
                         {{"team_id", created.value("id").toString()},
                          {"provider", "claude"},
                          {"members", QJsonArray{"not-a-member"}}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("not_member"));
        const QJsonObject unchanged = team(fixture, created);
        QVERIFY(unchanged.value("provider").toString().isEmpty());
        for (const QJsonValue& value : unchanged.value("members").toArray())
            QVERIFY(!value.toObject().contains("pending_switch"));
        QVERIFY(!updateKinds(fixture, created).contains("provider_set"));
    }

    // A member in a switched worktree cannot move to a provider without worktree switching:
    // it is reported with its reason, and the others move.
    void aBlockedMemberIsReportedAndTheOthersMove() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QJsonObject created = create(fixture).value("team").toObject();
        const QString moved = memberWith(created, "Developer 1");
        const QString other = memberWith(created, "Developer 2");
        StoredSession session = stored(fixture, moved);
        session.contextRevision = 2;
        QVERIFY(fixture.journal->upsertSession(session));
        fixture.restart();
        const QJsonObject answer = command(fixture, "team.provider.set",
                                           {{"team_id", created.value("id").toString()},
                                            {"provider", "cursor"},
                                            {"members", QJsonArray{moved, other}}});
        QVERIFY2(answer.value("ok").toBool(), qPrintable(text(answer)));
        const QJsonObject blocked = resultFor(answer, moved);
        QCOMPARE(blocked.value("status").toString(), QStringLiteral("skipped"));
        QCOMPARE(blocked.value("error_code").toString(),
                 QStringLiteral("context_switch_unsupported"));
        QVERIFY(!blocked.value("reason").toString().isEmpty());
        QCOMPARE(resultFor(answer, other).value("status").toString(), QStringLiteral("pending"));
        const QJsonObject after = answer.value("team").toObject();
        QVERIFY(!memberOf(after, moved).contains("pending_switch"));
        QVERIFY(memberOf(after, other).contains("pending_switch"));

        // The other developer moves to a switched worktree after its switch was asked: a
        // start_turn that is refused leaves the switch alone, the next accepted turn drops it
        // and runs where the member is.
        StoredSession later = stored(fixture, other);
        later.contextRevision = 2;
        QVERIFY(fixture.journal->upsertSession(later));
        fixture.restart();
        QCOMPARE(
            fixture.host
                ->handleCommand(request("session.start_turn",
                                        {{"prompt", "Old tab"}, {"expected_context_revision", 2}},
                                        other, 7),
                                "desktop-test")
                .value("error_code")
                .toString(),
            QStringLiteral("stale_generation"));
        QVERIFY(memberOf(team(fixture, created), other).contains("pending_switch"));
        QVERIFY(!updateKinds(fixture, created).contains("provider_switch_dropped"));
        QVERIFY(turn(fixture, other, "Carry on", {{"expected_context_revision", 2}}));
        QCOMPARE(stored(fixture, other).adapter, QStringLiteral("codex"));
        QVERIFY(!memberOf(team(fixture, created), other).contains("pending_switch"));
        QVERIFY(updateKinds(fixture, created).contains("provider_switch_dropped"));

        // Claude continues a conversation in a switched worktree: it takes the handoff.
        const QJsonObject claude = command(fixture, "team.provider.set",
                                           {{"team_id", created.value("id").toString()},
                                            {"provider", "claude"},
                                            {"members", QJsonArray{moved}}});
        QCOMPARE(resultFor(claude, moved).value("status").toString(), QStringLiteral("pending"));
    }
};

QTEST_GUILESS_MAIN(TeamProviderSwitchTest)
#include "TeamProviderSwitchTest.moc"
