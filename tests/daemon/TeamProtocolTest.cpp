// ADR 0114: the teams/1 protocol and the member scaffold, against a SessionHost with the
// scripted fake providers. No real provider is started and no turn runs.
#include "AgentTestFixture.h"
#include "teams/Team.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QProcess>
#include <QSet>
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

// Puts a `git` that fails first on PATH while it lives; with a log, each call adds a line.
struct FailingGit final {
    QByteArray path = qgetenv("PATH");
    bool installed = false;

    FailingGit(const QString& directory, const QString& log) {
        QFile script(directory + QStringLiteral("/git"));
        if (!QDir().mkpath(directory) || !script.open(QIODevice::WriteOnly))
            return;
        script.write("#!/bin/sh\n");
        if (!log.isEmpty())
            script.write("printf 'call\\n' >> '" + log.toUtf8() + "'\n");
        script.write("exit 1\n");
        script.close();
        installed = script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("PATH", directory.toUtf8() + ':' + path);
    }
    ~FailingGit() { qputenv("PATH", path); }
    FailingGit(const FailingGit&) = delete;
    FailingGit& operator=(const FailingGit&) = delete;
};

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
    // ADR 0115: every page of Updates says whether an older one follows, the largest page too.
    void everyPageOfUpdatesSaysWhetherAnOlderOneFollows() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QJsonValue teamId = create(fixture).value("team").toObject().value("id");
        const qint64 id = teamId.toString().toLongLong();
        QVERIFY(id > 0);
        const int written = command(fixture, "team.updates.list", {{"team_id", teamId}})
                                .value("updates")
                                .toArray()
                                .size();
        constexpr int added = 2 * ketplus::daemon::teams::maximumPage + 7;
        for (int index = 0; index < added; ++index)
            QVERIFY(fixture.journal->appendTeamUpdate(
                {.teamId = id,
                 .at = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
                 .actor = QStringLiteral("daemon"),
                 .kind = QStringLiteral("note"),
                 .text = QStringLiteral("Update %1").arg(index)}));
        const QJsonObject read = command(fixture, "team.get", {{"team_id", teamId}});
        QCOMPARE(read.value("updates").toArray().size(), ketplus::daemon::teams::defaultPage);
        QVERIFY(read.value("updates_has_more").toBool());
        QSet<qint64> seen;
        qint64 before = 0;
        QList<int> pages;
        for (bool more = true; more;) {
            QJsonObject payload{{"team_id", teamId}, {"limit", 1000}};
            if (before > 0)
                payload.insert("before_id", before);
            const QJsonObject page = command(fixture, "team.updates.list", payload);
            QVERIFY(page.value("ok").toBool());
            const QJsonArray updates = page.value("updates").toArray();
            QVERIFY(!updates.isEmpty());
            for (const auto& update : updates) {
                const qint64 updateId = update.toObject().value("id").toInteger();
                QVERIFY(!seen.contains(updateId));
                seen.insert(updateId);
                before = updateId;
            }
            pages.append(updates.size());
            more = page.value("has_more").toBool();
        }
        QCOMPARE(seen.size(), written + added);
        QCOMPARE(pages.first(), ketplus::daemon::teams::maximumPage);
        QCOMPARE(pages.size(), 3);
    }

    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void featureIsAdvertised() { QVERIFY(protocol::features(1).contains("teams/1")); }

    // A path that was no repository when it was first asked about is asked again (#473): its
    // teams are found once it is one.
    void aPathThatBecomesARepositoryFindsItsTeams() {
        if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
            QSKIP("git is not installed");
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString inside = fixture.directory.filePath(QStringLiteral("apps"));
        QVERIFY(QDir().mkpath(inside));
        QVERIFY(command(fixture, "team.list", {{"repository", inside}})
                    .value("teams")
                    .toArray()
                    .isEmpty());
        QProcess git;
        git.setWorkingDirectory(fixture.directory.path());
        git.start(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("--quiet")});
        QVERIFY(git.waitForFinished(15000));
        QCOMPARE(git.exitCode(), 0);
        // The next command arrives in another turn of the event loop.
        QCoreApplication::processEvents();
        QVERIFY2(create(fixture).value("ok").toBool(), "the team is created in the repository");
        QCOMPARE(
            command(fixture, "team.list", {{"repository", inside}}).value("teams").toArray().size(),
            1);
    }

    // A git that does not answer is asked once for a path within a command, however many of
    // a team's members share it, and again by the next command.
    void aFailedLookupIsAskedOncePerCommand() {
#ifdef Q_OS_WIN
        QSKIP("the failing git is a shell script");
#endif
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString log = fixture.directory.filePath(QStringLiteral("git-calls"));
        const FailingGit git(fixture.directory.filePath(QStringLiteral("bin")), log);
        QVERIFY(git.installed);
        const QString inside = fixture.directory.filePath(QStringLiteral("apps"));
        QVERIFY(QDir().mkpath(inside));
        const auto list = [&] {
            return command(fixture, "team.list", {{"repository", inside}}).value("ok").toBool();
        };
        QVERIFY(list());
        QVERIFY(list());
        QCOMPARE(logText(log).count('\n'), 1);
        QCoreApplication::processEvents();
        QVERIFY(list());
        QCOMPARE(logText(log).count('\n'), 2);

        const QJsonObject made = create(fixture);
        QVERIFY2(made.value("ok").toBool(), qPrintable(text(made)));
        QCoreApplication::processEvents();
        const qsizetype before = logText(log).count('\n');
        QVERIFY(
            command(fixture, "team.get", {{"team_id", made.value("team").toObject().value("id")}})
                .value("ok")
                .toBool());
        QVERIFY2(logText(log).count('\n') - before <= 1, "one team.get asks git at most once");
    }

    // A team created while git did not answer is filed under the path it was given. It stays
    // found there once git answers again and names the repository's root.
    void aTeamCreatedWhileGitFailedStaysFound() {
        const QString realGit = QStandardPaths::findExecutable(QStringLiteral("git"));
        if (realGit.isEmpty())
            QSKIP("git is not installed");
#ifdef Q_OS_WIN
        QSKIP("the failing git is a shell script");
#endif
        Fixture fixture;
        QVERIFY(fixture.open());
        QProcess init;
        init.setWorkingDirectory(fixture.directory.path());
        init.start(realGit, {QStringLiteral("init"), QStringLiteral("--quiet")});
        QVERIFY(init.waitForFinished(15000));
        QCOMPARE(init.exitCode(), 0);
        const QString inside = fixture.directory.filePath(QStringLiteral("apps"));
        QVERIFY(QDir().mkpath(inside));
        const auto listed = [&] {
            return command(fixture, "team.list", {{"repository", inside}})
                .value("teams")
                .toArray()
                .size();
        };
        {
            const FailingGit git(fixture.directory.filePath(QStringLiteral("bin")), QString());
            QVERIFY(git.installed);
            const QJsonObject made = command(fixture, "team.create",
                                             {{"repository", inside},
                                              {"name", "Checkout"},
                                              {"goal", "Ship the checkout"},
                                              {"integration_branch", "integration"},
                                              {"merge_policy", "leader"},
                                              {"plan", plan()}});
            QVERIFY2(made.value("ok").toBool(), qPrintable(text(made)));
            QCOMPARE(listed(), 1);
        }
        QCoreApplication::processEvents();
        QCOMPARE(listed(), 1);
    }

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
