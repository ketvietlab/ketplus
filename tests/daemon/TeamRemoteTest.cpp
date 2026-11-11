// ADR 0116: team commands from a paired phone, at the session host. They meet the rules a
// local client meets, are held to the fields of §4 and answer with the read model of §6,
// checked against the contract's fixture. No real provider is started.
#include "AgentTestFixture.h"
#include "teams/TeamRemoteView.h"

#include <QFile>
#include <QProcess>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

using namespace agent_test;
namespace remote = ketplus::daemon::teams::remote;

namespace {

void git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", arguments);
    QVERIFY(process.waitForFinished(15000));
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
}

QJsonObject contract() {
    QFile file(QStringLiteral(KETPLUS_TEST_FIXTURES "/team-remote-v1.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

// The fields that may be null in place of their value.
const QSet<QString> nullable{"now", "recommended", "pull_request_number", "owner_number"};
// An actor names a member with its number, or the user or the daemon with its role only.
const QSet<QString> actors{"raised_by", "actor"};

// The answer has the keys the fixture's object has, recursively, with the same JSON type.
void sameShape(const QJsonValue& actual, const QJsonValue& expected, const QString& path) {
    const QString key = path.section('.', -1).section('[', 0, 0);
    if (expected.isNull() || (actual.isNull() && nullable.contains(key)))
        return;
    if (actors.contains(key)) {
        QVERIFY2(actual.toObject().value("role").isString(), qPrintable(path));
        return;
    }
    QVERIFY2(actual.type() == expected.type() || (expected.isDouble() && actual.isDouble()),
             qPrintable(path + ": " + QString::number(actual.type()) + " for " +
                        QString::number(expected.type())));
    if (expected.isObject()) {
        const QJsonObject want = expected.toObject();
        const QJsonObject have = actual.toObject();
        const QStringList wantList = want.keys();
        const QStringList haveList = have.keys();
        const QSet<QString> wantKeys(wantList.cbegin(), wantList.cend());
        const QSet<QString> haveKeys(haveList.cbegin(), haveList.cend());
        QVERIFY2(wantKeys == haveKeys,
                 qPrintable(path + ": " + QStringList(haveKeys.values()).join(',') + " for " +
                            QStringList(wantKeys.values()).join(',')));
        for (const QString& key : want.keys())
            sameShape(have.value(key), want.value(key), path + "." + key);
    } else if (expected.isArray() && !expected.toArray().isEmpty() && !actual.toArray().isEmpty()) {
        sameShape(actual.toArray().first(), expected.toArray().first(), path + "[0]");
    }
}

struct Team final {
    QString id;
    QString leader;
    QString developer;
    QString reviewer;
};

} // namespace

class TeamRemoteTest final : public QObject {
    Q_OBJECT

  private:
    remote::Ids ids{[](const QString& path) { return "repo:" + QFileInfo(path).fileName(); },
                    [](const QString& path) { return "wt:" + QFileInfo(path).fileName(); }};
    int sent_ = 0;

    QJsonObject phone(Fixture& fixture, const QString& type, const QJsonObject& payload,
                      const QString& commandId = {}) {
        QJsonObject message = request(type, payload);
        message.insert("message_id", commandId.isEmpty()
                                         ? QStringLiteral("team-command-%1").arg(++sent_)
                                         : commandId);
        QJsonObject answer;
        bool answered = false;
        fixture.host->remoteTeamCommand(message, ids, [&](const QJsonObject& result) {
            answer = result;
            answered = true;
        });
        [&] { QTRY_VERIFY_WITH_TIMEOUT(answered, 10000); }();
        return answer;
    }

    QJsonObject local(Fixture& fixture, const QString& type, const QJsonObject& payload) {
        return fixture.host->handleCommand(request(type, payload), "desktop-test");
    }

    QString repository(Fixture& fixture) {
        const QString path = fixture.directory.filePath("shop");
        if (QDir(path).exists())
            return path;
        QDir().mkpath(path);
        git(path, {"init", "-q", "-b", "integration"});
        git(path, {"-c", "user.email=t@example.com", "-c", "user.name=T", "commit", "-q",
                   "--allow-empty", "-m", "start"});
        return path;
    }

    QJsonObject createPayload(Fixture& fixture) {
        return {{"repository", repository(fixture)},
                {"name", "Shop"},
                {"goal", "Ship the shop"},
                {"integration_branch", "integration"},
                {"merge_policy", "leader"},
                {"plan", QJsonObject{{"leader", QJsonObject{{"provider", "claude"}}},
                                     {"developers",
                                      QJsonObject{{"count", 1},
                                                  {"provider", "codex"},
                                                  {"members", QJsonArray{QJsonObject{
                                                                  {"paths", QJsonArray{"web"}}}}}}},
                                     {"reviewer", QJsonObject{{"provider", "claude"}}}}}};
    }

    Team create(Fixture& fixture) {
        const QJsonObject created = phone(fixture, "team.create", createPayload(fixture));
        const QJsonObject team = created.value("team").toObject();
        Team result{.id = team.value("id").toString()};
        for (const QJsonValue& value : team.value("members").toArray()) {
            const QJsonObject member = value.toObject();
            const QString role = member.value("role").toString();
            (role == "leader"     ? result.leader
             : role == "reviewer" ? result.reviewer
                                  : result.developer) = member.value("session_id").toString();
        }
        return result;
    }

    qint64 revision(Fixture& fixture, const QString& team) {
        return phone(fixture, "team.get", {{"team_id", team}})
            .value("team")
            .toObject()
            .value("revision")
            .toInteger();
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // §6: each read answers with the fields of the contract's fixture, and no local path.
    void theReadModelHasTheContractsShape() {
        const QJsonObject fixtureJson = contract();
        QVERIFY(!fixtureJson.isEmpty());
        Fixture fixture;
        QVERIFY(fixture.open());
        const Team team = create(fixture);
        QVERIFY(!team.id.isEmpty());
        QVERIFY(fixture
                    .agents(team.leader, "agents.team_task_create",
                            {{"title", "Cart"}, {"lane", "web"}, {"criteria", QJsonArray{"Adds"}}})
                    .value("ok")
                    .toBool());
        QVERIFY(fixture
                    .agents(team.leader, "agents.team_decision_raise",
                            {{"question", "Start?"}, {"choices", QJsonArray{"Start", "Wait"}}})
                    .value("ok")
                    .toBool());

        const QJsonObject list = phone(fixture, "team.list", {});
        sameShape(list, fixtureJson.value("directory").toObject().value("result"), "directory");
        QCOMPARE(list.value("teams").toArray().first().toObject().value("repository_id"),
                 QJsonValue("repo:shop"));

        const QJsonObject snapshot = phone(fixture, "team.get", {{"team_id", team.id}});
        sameShape(snapshot, fixtureJson.value("snapshot").toObject().value("result"), "snapshot");
        const QJsonObject summary = snapshot.value("team").toObject();
        QCOMPARE(summary.value("turns_left").toInt(),
                 summary.value("turn_budget").toInt() - summary.value("automatic_turns").toInt());
        const QJsonObject decision = summary.value("decisions").toArray().first().toObject();
        QCOMPARE(decision.value("raised_by").toObject().value("role"), QJsonValue("leader"));
        QCOMPARE(decision.value("delivers_to").toArray().first().toObject().value("session_id"),
                 QJsonValue(team.leader));

        const QJsonObject detail =
            phone(fixture, "team.task.get", {{"team_id", team.id}, {"task_id", "T-1"}});
        sameShape(detail, fixtureJson.value("taskDetail").toObject().value("result"), "taskDetail");

        const QJsonObject unchanged =
            phone(fixture, "team.get",
                  {{"team_id", team.id}, {"known_revision", summary.value("revision")}});
        sameShape(unchanged, fixtureJson.value("unchanged").toObject().value("result"),
                  "unchanged");

        // The phone never receives a local path, nor a tag.
        for (const QJsonObject& answer : {list, snapshot, detail}) {
            const QByteArray bytes = QJsonDocument(answer).toJson(QJsonDocument::Compact);
            QVERIFY2(!bytes.contains(fixture.directory.path().toUtf8()), bytes.constData());
            QVERIFY(!bytes.contains("\"tags\""));
            QVERIFY(!bytes.contains("\"repository\""));
        }
    }

    // §4: a refusal of the local rules reaches the phone with the same code, and a phone
    // sends only the fields the ADR lists.
    void aPhoneMeetsTheLocalRulesAndOnlyItsFields() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const Team team = create(fixture);
        const qint64 seen = revision(fixture, team.id);
        const auto code = [&](const QString& type, const QJsonObject& payload) {
            return phone(fixture, type, payload).value("error_code").toString();
        };
        QCOMPARE(code("team.member.remove", {{"team_id", team.id},
                                             {"session_id", team.developer},
                                             {"expected_revision", seen}}),
                 QStringLiteral("last_developer"));
        QCOMPARE(
            code("team.member.join",
                 {{"team_id", team.id}, {"session_id", "missing"}, {"expected_revision", seen}}),
            QStringLiteral("session_not_found"));
        QCOMPARE(code("team.update",
                      {{"team_id", team.id}, {"name", "Other"}, {"expected_revision", seen}}),
                 QStringLiteral("invalid_payload"));
        QCOMPARE(code("team.update", {{"team_id", team.id}, {"paused", true}}),
                 QStringLiteral("invalid_payload"));
        QCOMPARE(code("team.delete", {{"team_id", team.id}, {"expected_revision", seen}}),
                 QStringLiteral("confirmation_required"));
        QCOMPARE(code("team.task.handover", {{"team_id", team.id},
                                             {"task_id", "T-1"},
                                             {"owner", team.developer},
                                             {"reason", " "},
                                             {"expected_revision", seen}}),
                 QStringLiteral("invalid_payload"));
        QCOMPARE(code("team.task.create", {{"team_id", team.id}, {"title", "x"}}),
                 QStringLiteral("unknown_operation"));
        QCOMPARE(code("team.get", {{"team_id", "999"}}), QStringLiteral("team_not_found"));
        QCOMPARE(code("team.create",
                      [&] {
                          QJsonObject payload = createPayload(fixture);
                          payload.insert("shared_paths", QJsonArray{"docs"});
                          return payload;
                      }()),
                 QStringLiteral("invalid_payload"));

        QVERIFY(fixture
                    .agents(team.leader, "agents.team_decision_raise",
                            {{"question", "Go?"}, {"choices", QJsonArray{"Go", "Stop"}}})
                    .value("ok")
                    .toBool());
        const QJsonObject answerBase{{"team_id", team.id},
                                     {"decision_id", "D-1"},
                                     {"expected_revision", revision(fixture, team.id)}};
        QJsonObject both = answerBase;
        both.insert("choice", "Go");
        both.insert("text", "Go now");
        QCOMPARE(code("team.decision.answer", both), QStringLiteral("invalid_payload"));
        QJsonObject other = answerBase;
        other.insert("choice", "Maybe");
        QCOMPARE(code("team.decision.answer", other), QStringLiteral("invalid_payload"));
        QJsonObject go = answerBase;
        go.insert("choice", "Go");
        const QJsonObject answered = phone(fixture, "team.decision.answer", go);
        QVERIFY2(answered.value("ok").toBool(), qPrintable(text(answered)));
        // Answered again from the same view, the decision moved since; from the new one, it
        // is closed.
        QCOMPARE(code("team.decision.answer", go), QStringLiteral("stale_team"));
        go.insert("expected_revision", revision(fixture, team.id));
        QCOMPARE(code("team.decision.answer", go), QStringLiteral("decision_closed"));
    }

    // §4: a change is refused only when the part it acts on moved after the phone looked.
    void aChangeIsFencedOnItsOwnPart() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const Team team = create(fixture);
        QVERIFY(fixture
                    .agents(team.leader, "agents.team_decision_raise",
                            {{"question", "Go?"}, {"choices", QJsonArray{"Go", "Stop"}}})
                    .value("ok")
                    .toBool());
        const qint64 seen = revision(fixture, team.id);
        // A task the leader creates moves the team, not the decision nor the settings.
        QVERIFY(fixture.agents(team.leader, "agents.team_task_create", {{"title", "Cart"}})
                    .value("ok")
                    .toBool());
        QVERIFY(revision(fixture, team.id) > seen);
        const QJsonObject paused =
            phone(fixture, "team.update",
                  {{"team_id", team.id}, {"paused", true}, {"expected_revision", seen}});
        QVERIFY2(paused.value("ok").toBool(), qPrintable(text(paused)));
        QVERIFY(paused.value("team").toObject().value("paused").toBool());
        // The settings moved since `seen`: a second change drawn from it is stale.
        const QJsonObject stale =
            phone(fixture, "team.update",
                  {{"team_id", team.id}, {"merge_policy", "user"}, {"expected_revision", seen}});
        QCOMPARE(stale.value("error_code").toString(), QStringLiteral("stale_team"));
        QCOMPARE(stale.value("team").toObject().value("revision").toInteger(),
                 revision(fixture, team.id));
        // The decision did not move.
        const QJsonObject answered = phone(fixture, "team.decision.answer",
                                           {{"team_id", team.id},
                                            {"decision_id", "D-1"},
                                            {"choice", "Stop"},
                                            {"expected_revision", seen}});
        QVERIFY2(answered.value("ok").toBool(), qPrintable(text(answered)));
        // A deletion depends on the members: one that left since refuses it.
        const qint64 beforeMembers = revision(fixture, team.id);
        const QJsonObject added = phone(fixture, "team.member.add",
                                        {{"team_id", team.id},
                                         {"name", "API"},
                                         {"paths", QJsonArray{"api"}},
                                         {"provider", "codex"},
                                         {"expected_revision", beforeMembers}});
        QVERIFY2(added.value("ok").toBool(), qPrintable(text(added)));
        QCOMPARE(phone(fixture, "team.delete",
                       {{"team_id", team.id},
                        {"confirmation", "DELETE"},
                        {"expected_revision", beforeMembers}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("stale_team"));
        // Local clients send no expected_revision and are not fenced.
        QVERIFY(local(fixture, "team.update", {{"team_id", team.id}, {"paused", false}})
                    .value("ok")
                    .toBool());
    }

    // §5: `full` needs the acknowledgement; a repeated delivery of team.create acts once.
    void creatingATeamNeedsTheAcknowledgementAndActsOnce() {
        Fixture fixture;
        QVERIFY(fixture.open());
        QJsonObject full = createPayload(fixture);
        full.insert("execution_policy", "full");
        QCOMPARE(phone(fixture, "team.create", full).value("error_code").toString(),
                 QStringLiteral("execution_policy_denied"));
        full.insert("risk_acknowledged", true);
        const QJsonObject first = phone(fixture, "team.create", full, "team-command-create");
        QVERIFY2(first.value("ok").toBool(), qPrintable(text(first)));
        QCOMPARE(first.value("team").toObject().value("execution_policy"), QJsonValue("full"));
        const int conversations =
            local(fixture, "session.list", {}).value("sessions").toArray().size();
        const QJsonObject again = phone(fixture, "team.create", full, "team-command-create");
        QCOMPARE(again, first);
        QCOMPARE(local(fixture, "session.list", {}).value("sessions").toArray().size(),
                 conversations);
        QCOMPARE(phone(fixture, "team.list", {}).value("teams_total").toInt(), 1);
    }

    // §3: each member carries {id, revision, role}; a member that leaves and a deleted team
    // lose it, and the change is announced.
    void theMetadataFollowsTheTeam() {
        Fixture fixture;
        QVERIFY(fixture.open());
        QSignalSpy changes(fixture.host.get(), &SessionHost::teamEvent);
        const Team team = create(fixture);
        QVERIFY(!changes.isEmpty());
        QHash<QString, QJsonObject> metadata = fixture.host->teamMetadata();
        QCOMPARE(metadata.size(), 3);
        QCOMPARE(metadata.value(team.developer).value("role"), QJsonValue("developer"));
        QCOMPARE(metadata.value(team.leader).value("id"), QJsonValue(team.id));
        const qint64 before = metadata.value(team.leader).value("revision").toInteger();
        const QJsonObject added = phone(fixture, "team.member.add",
                                        {{"team_id", team.id},
                                         {"name", "API"},
                                         {"paths", QJsonArray{"api"}},
                                         {"provider", "codex"},
                                         {"expected_revision", revision(fixture, team.id)}});
        QVERIFY2(added.value("ok").toBool(), qPrintable(text(added)));
        metadata = fixture.host->teamMetadata();
        QCOMPARE(metadata.size(), 4);
        QVERIFY(metadata.value(team.leader).value("revision").toInteger() > before);
        const QJsonObject removed = phone(fixture, "team.member.remove",
                                          {{"team_id", team.id},
                                           {"session_id", team.developer},
                                           {"expected_revision", revision(fixture, team.id)}});
        QVERIFY2(removed.value("ok").toBool(), qPrintable(text(removed)));
        QVERIFY(!fixture.host->teamMetadata().contains(team.developer));
        changes.clear();
        const QJsonObject deleted = phone(fixture, "team.delete",
                                          {{"team_id", team.id},
                                           {"confirmation", "DELETE"},
                                           {"expected_revision", revision(fixture, team.id)}});
        QVERIFY2(deleted.value("ok").toBool(), qPrintable(text(deleted)));
        QVERIFY(fixture.host->teamMetadata().isEmpty());
        QVERIFY(!changes.isEmpty());
        QCOMPARE(phone(fixture, "team.list", {}).value("teams").toArray().size(), 0);
    }

    // §6: a result stays within 384 KiB; answered decisions, then Updates, then done tasks go.
    void aResultIsShedInOrderOrRefused() {
        const QString big(4000, QLatin1Char('x'));
        QJsonArray decisions;
        for (int index = 0; index < 40; ++index)
            decisions.append(
                QJsonObject{{"state", index == 0 ? "open" : "answered"}, {"context", big}});
        QJsonArray updates;
        for (int index = 0; index < 40; ++index)
            updates.append(QJsonObject{{"text", big}});
        QJsonArray tasks;
        for (int index = 0; index < 60; ++index)
            tasks.append(QJsonObject{{"state", index < 2 ? "todo" : "done"}, {"title", big}});
        QJsonObject result{{"ok", true},
                           {"team", QJsonObject{{"decisions", decisions}, {"tasks", tasks}}},
                           {"updates", updates}};
        auto shed = remote::bounded(result);
        QVERIFY(shed);
        QCOMPARE(QJsonDocument(*shed).toJson(QJsonDocument::Compact).size() <=
                     remote::maximumResultBytes,
                 true);
        const QJsonObject team = shed->value("team").toObject();
        QVERIFY(team.value("decisions_truncated").toBool());
        QCOMPARE(team.value("decisions").toArray().size(), 1);
        QVERIFY(shed->value("updates_truncated").toBool());

        // Even shed, the open parts do not fit.
        QJsonArray open;
        for (int index = 0; index < 200; ++index)
            open.append(QJsonObject{{"state", "todo"}, {"title", big}});
        result.insert("team", QJsonObject{{"tasks", open}});
        QVERIFY(!remote::bounded(result));
        QVERIFY(remote::pullRequestNumber("https://example.invalid/pull/71") == 71);
        QVERIFY(!remote::pullRequestNumber("draft"));
    }
};

QTEST_GUILESS_MAIN(TeamRemoteTest)
#include "TeamRemoteTest.moc"
