// ADR 0114: the teams/1 protocol and the member scaffold, against a SessionHost with the
// scripted fake providers. No real provider is started and no turn runs.
#include "AgentTestFixture.h"

#include <QDir>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

using namespace agent_test;

namespace {

QJsonObject plan(const QString& reviewerProvider = QStringLiteral("claude"), int developers = 2) {
    return {{"leader", QJsonObject{{"provider", "claude"}, {"model", "opus"}, {"effort", "high"}}},
            {"developers",
             QJsonObject{{"count", developers},
                         {"provider", "codex"},
                         {"model", "gpt-5"},
                         {"effort", "medium"},
                         {"members", QJsonArray{QJsonObject{{"paths", QJsonArray{"apps/web"}}},
                                                QJsonObject{{"paths", QJsonArray{"apps/api"}}}}}}},
            {"reviewer", QJsonObject{{"provider", reviewerProvider}, {"model", "sonnet"}}}};
}

} // namespace

class TeamProtocolTest final : public QObject {
    Q_OBJECT

  private:
    QJsonObject command(Fixture& fixture, const QString& type, const QJsonObject& payload,
                        const QString& client = QStringLiteral("desktop-test")) {
        return fixture.host->handleCommand(request(type, payload), client);
    }

    QJsonObject create(Fixture& fixture, const QJsonObject& teamPlan = plan()) {
        return command(fixture, "team.create",
                       {{"repository", fixture.directory.path()},
                        {"name", "Checkout"},
                        {"goal", "Ship the checkout"},
                        {"integration_branch", "integration"},
                        {"merge_policy", "leader"},
                        {"plan", teamPlan}});
    }

    int sessions(Fixture& fixture) {
        return command(fixture, "session.list", {}).value("sessions").toArray().size();
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void featureIsAdvertised() { QVERIFY(protocol::features(1).contains("teams/1")); }

    void createStartsFourNamedMembersAndSurvivesRestart() {
        Fixture fixture;
        QVERIFY(fixture.open());
        QSignalSpy events(fixture.host.get(), &SessionHost::teamEvent);
        const QJsonObject created = create(fixture);
        QVERIFY2(created.value("ok").toBool(), qPrintable(text(created)));
        const QJsonObject team = created.value("team").toObject();
        const QJsonArray members = team.value("members").toArray();
        QCOMPARE(members.size(), 4);
        QCOMPARE(members.at(0).toObject().value("name").toString(),
                 QStringLiteral("Lead Checkout"));
        QCOMPARE(members.at(0).toObject().value("provider").toString(), QStringLiteral("claude"));
        QCOMPARE(members.at(0).toObject().value("model").toString(), QStringLiteral("opus"));
        QCOMPARE(members.at(0).toObject().value("effort").toString(), QStringLiteral("high"));
        QCOMPARE(members.at(1).toObject().value("name").toString(), QStringLiteral("Developer 1"));
        QCOMPARE(members.at(1).toObject().value("provider").toString(), QStringLiteral("codex"));
        QCOMPARE(members.at(1).toObject().value("paths").toArray().first().toString(),
                 QStringLiteral("apps/web"));
        QCOMPARE(members.at(2).toObject().value("name").toString(), QStringLiteral("Developer 2"));
        QCOMPARE(members.at(3).toObject().value("name").toString(), QStringLiteral("Reviewer"));
        QCOMPARE(members.at(3).toObject().value("role").toString(), QStringLiteral("reviewer"));
        QVERIFY(!members.at(0).toObject().value("number").toString().isEmpty());
        QCOMPARE(sessions(fixture), 4);
        // Starting a member starts no turn; only the leader gets the goal (#458).
        for (int index = 1; index < members.size(); ++index)
            QCOMPARE(fixture.count(members.at(index).toObject().value("session_id").toString(),
                                   "user.message"),
                     0);

        QVERIFY(events.size() >= 1);
        QCOMPARE(events.first().at(0).toString(), QStringLiteral("team.changed"));

        fixture.restart();
        const QJsonObject listed =
            command(fixture, "team.list", {{"repository", fixture.directory.path()}});
        QCOMPARE(listed.value("teams").toArray().size(), 1);
        const QJsonObject again = listed.value("teams").toArray().first().toObject();
        QCOMPARE(again.value("id"), team.value("id"));
        QCOMPARE(again.value("members").toArray().size(), 4);
        QCOMPARE(again.value("members").toArray().at(0).toObject().value("title").toString(),
                 QStringLiteral("Lead Checkout"));
    }

    // The team's permissions are the execution policy of the leader and the developers it
    // starts, at creation and later; the reviewer keeps a conversation's default.
    void theTeamsPermissionsReachItsLeaderAndDevelopers() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const auto withPolicy = [&](const QString& policy, const bool acknowledged = true) {
            return command(fixture, "team.create",
                           {{"repository", fixture.directory.path()},
                            {"name", "Checkout"},
                            {"integration_branch", "integration"},
                            {"execution_policy", policy},
                            {"risk_acknowledged", acknowledged},
                            {"plan", plan()}});
        };
        // Full access needs its acknowledgement, as it does for one conversation.
        QCOMPARE(withPolicy("full", false).value("error_code").toString(),
                 QStringLiteral("execution_policy_denied"));
        QCOMPARE(sessions(fixture), 0);
        QCOMPARE(withPolicy("everything").value("error_code").toString(),
                 QStringLiteral("invalid_payload"));
        QCOMPARE(sessions(fixture), 0);
        const QJsonObject created = withPolicy("full");
        QVERIFY2(created.value("ok").toBool(), qPrintable(text(created)));
        const QJsonObject team = created.value("team").toObject();
        QCOMPARE(team.value("execution_policy").toString(), QStringLiteral("full"));
        const auto policyOf = [&](const QString& sessionId) {
            for (const StoredSession& stored : fixture.journal->sessions())
                if (stored.id == sessionId)
                    return stored.executionPolicy;
            return QString{};
        };
        for (const auto& value : team.value("members").toArray()) {
            const QJsonObject member = value.toObject();
            QCOMPARE(policyOf(member.value("session_id").toString()),
                     member.value("role").toString() == "reviewer" ? QStringLiteral("ask")
                                                                   : QStringLiteral("full"));
        }
        // A developer the team starts later gets the same.
        const QJsonObject added = command(fixture, "team.member.add",
                                          {{"team_id", team.value("id").toString()},
                                           {"name", "Docs writer"},
                                           {"lane", "Docs"},
                                           {"paths", QJsonArray{"docs"}},
                                           {"provider", "codex"}});
        QVERIFY2(added.value("ok").toBool(), qPrintable(text(added)));
        const QJsonArray members = added.value("team").toObject().value("members").toArray();
        bool found = false;
        for (const auto& value : members)
            if (value.toObject().value("name").toString() == "Docs writer") {
                found = true;
                QCOMPARE(policyOf(value.toObject().value("session_id").toString()),
                         QStringLiteral("full"));
            }
        QVERIFY(found);
        // A team made without the setting asks, as before.
        fixture.restart();
        QCOMPARE(command(fixture, "team.list", {})
                     .value("teams")
                     .toArray()
                     .first()
                     .toObject()
                     .value("execution_policy")
                     .toString(),
                 QStringLiteral("full"));
    }

    void refusedCreateLeavesNothingBehind() {
        Fixture fixture;
        QVERIFY(fixture.open());
        QJsonObject result = create(fixture, plan(QStringLiteral("codex")));
        QCOMPARE(result.value("error_code").toString(), QStringLiteral("reviewer_unsupported"));
        QCOMPARE(result.value("role").toString(), QStringLiteral("reviewer"));
        QCOMPARE(sessions(fixture), 0);
        result = create(fixture, plan(QStringLiteral("claude"), 6));
        QCOMPARE(result.value("error_code").toString(), QStringLiteral("team_full"));
        QJsonObject overlapping = plan();
        QJsonObject developers = overlapping.value("developers").toObject();
        developers.insert("members", QJsonArray{QJsonObject{{"paths", QJsonArray{"apps"}}},
                                                QJsonObject{{"paths", QJsonArray{"apps/api"}}}});
        overlapping.insert("developers", developers);
        result = create(fixture, overlapping);
        QCOMPARE(result.value("error_code").toString(), QStringLiteral("lane_overlap"));
        QCOMPARE(sessions(fixture), 0);
        QVERIFY(command(fixture, "team.list", {}).value("teams").toArray().isEmpty());
    }

    void membersAreAddedJoinedChangedAndRemoved() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QJsonObject team = create(fixture).value("team").toObject();
        const QString id = team.value("id").toString();
        QJsonObject added = command(fixture, "team.member.add",
                                    {{"team_id", id},
                                     {"name", "Docs writer"},
                                     {"lane", "Docs"},
                                     {"paths", QJsonArray{"docs"}},
                                     {"provider", "codex"}});
        QVERIFY2(added.value("ok").toBool(), qPrintable(text(added)));
        QCOMPARE(added.value("team").toObject().value("members").toArray().size(), 5);
        QCOMPARE(command(fixture, "team.member.add", {{"team_id", id}, {"provider", "codex"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("invalid_payload"));
        // An existing conversation joins as a developer.
        const QString loner = fixture.start("codex", "Fix the footer");
        QVERIFY(!loner.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.host->handleCommand(request("session.list"), "t")
                                      .value("sessions")
                                      .toArray()
                                      .size(),
                                  6, 5000);
        QJsonObject joined =
            command(fixture, "team.member.join",
                    {{"team_id", id}, {"session_id", loner}, {"paths", QJsonArray{"web/footer"}}});
        QVERIFY2(joined.value("ok").toBool(), qPrintable(text(joined)));
        const auto developerCount = [](const QJsonObject& value) {
            int total = 0;
            for (const auto& member : value.value("members").toArray())
                total += member.toObject().value("role").toString() == "developer" ? 1 : 0;
            return total;
        };
        QCOMPARE(developerCount(joined.value("team").toObject()), 4);
        QCOMPARE(command(fixture, "team.member.join", {{"team_id", id}, {"session_id", loner}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("already_member"));
        // A member's own reviewer setting and archiving are refused.
        QCOMPARE(fixture.host
                     ->handleCommand(
                         request("session.reviewer.set", {{"new_provider", "claude"}}, loner, 1),
                         "desktop-test")
                     .value("error_code")
                     .toString(),
                 QStringLiteral("team_member"));
        QCOMPARE(
            fixture.host
                ->handleCommand(request("session.set_archived", {{"archived", true}}, loner, 1),
                                "desktop-test")
                .value("error_code")
                .toString(),
            QStringLiteral("team_member"));
        QJsonObject changed =
            command(fixture, "team.member.update",
                    {{"team_id", id}, {"session_id", loner}, {"paths", QJsonArray{"apps/web/x"}}});
        QCOMPARE(changed.value("error_code").toString(), QStringLiteral("lane_overlap"));
        changed = command(fixture, "team.member.update",
                          {{"team_id", id},
                           {"session_id", loner},
                           {"lane", "Footer"},
                           {"paths", QJsonArray{"web/footer", "web/header"}},
                           {"reason", "header too"}});
        QVERIFY2(changed.value("ok").toBool(), qPrintable(text(changed)));
        const QString leader =
            team.value("members").toArray().first().toObject().value("session_id").toString();
        QCOMPARE(command(fixture, "team.member.remove", {{"team_id", id}, {"session_id", leader}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("role_required"));
        QVERIFY(command(fixture, "team.member.remove", {{"team_id", id}, {"session_id", loner}})
                    .value("ok")
                    .toBool());
        // Removed, it is an ordinary conversation again.
        QVERIFY(fixture.host
                    ->handleCommand(request("session.set_archived", {{"archived", true}}, loner, 1),
                                    "desktop-test")
                    .value("ok")
                    .toBool());
        const QJsonObject read = command(fixture, "team.get", {{"team_id", id}});
        QVERIFY(read.value("updates").toArray().size() >= 4);
        // Update and delete; the conversations stay.
        QVERIFY(command(fixture, "team.update",
                        {{"team_id", id}, {"merge_policy", "user"}, {"goal", "New"}})
                    .value("team")
                    .toObject()
                    .value("merge_policy")
                    .toString() == "user");
        QCOMPARE(command(fixture, "team.update", {{"team_id", id}, {"merge_policy", "nobody"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("invalid_payload"));
        QSignalSpy events(fixture.host.get(), &SessionHost::teamEvent);
        QVERIFY(command(fixture, "team.delete", {{"team_id", id}}).value("ok").toBool());
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().at(0).toString(), QStringLiteral("team.removed"));
        QCOMPARE(sessions(fixture), 6);
        QCOMPARE(command(fixture, "team.get", {{"team_id", id}}).value("error_code").toString(),
                 QStringLiteral("team_not_found"));
    }

    void otherRepositoriesAndRemoteClientsAreRefused() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString id = create(fixture).value("team").toObject().value("id").toString();
        QTemporaryDir other;
        const QString stranger = fixture.start("codex", "Elsewhere", {}, other.path());
        QVERIFY(!stranger.isEmpty());
        QCOMPARE(command(fixture, "team.member.join", {{"team_id", id}, {"session_id", stranger}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("other_repository"));
        QCOMPARE(command(fixture, "team.list", {}, QStringLiteral("cloud-control-v2"))
                     .value("error_code")
                     .toString(),
                 QStringLiteral("forbidden"));
    }
};

QTEST_GUILESS_MAIN(TeamProtocolTest)
#include "TeamProtocolTest.moc"
