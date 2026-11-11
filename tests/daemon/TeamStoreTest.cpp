// ADR 0114: the team store and the membership rules, on a real journal with a fake runtime.
// No provider is started.
#include "storage/LocalJournal.h"
#include "teams/TeamCoordinator.h"

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace ketplus::daemon;
using namespace ketplus::daemon::teams;

namespace {

class FakeHost final : public TeamHostPort {
  public:
    std::optional<TeamConversation> teamConversation(const QString& sessionId) const override {
        const auto it = conversations.constFind(sessionId);
        if (it == conversations.cend())
            return std::nullopt;
        return it.value();
    }

    void add(const QString& id, const QString& provider = QStringLiteral("codex"),
             const QString& repository = QStringLiteral("/repo")) {
        conversations.insert(id, {.sessionId = id,
                                  .generation = 1,
                                  .number = QString::number(1000 + conversations.size()),
                                  .title = id,
                                  .provider = provider,
                                  .worktree = repository,
                                  .repository = repository,
                                  .idle = true});
    }

    QHash<QString, TeamConversation> conversations;
};

Member member(const QString& session, const QString& role, const QString& name,
              const QStringList& paths = {}) {
    return {.sessionId = session,
            .role = role,
            .name = name,
            .lane = role == role::developer ? name : QString{},
            .paths = paths};
}

Team draft(const QList<Member>& members) {
    Team team;
    team.repository = QStringLiteral("/repo");
    team.name = QStringLiteral("Checkout");
    team.goal = QStringLiteral("Ship the checkout");
    team.integrationBranch = QStringLiteral("integration");
    team.members = members;
    return team;
}

Team standard() {
    return draft({member("lead", role::leader, "Lead Checkout"),
                  member("dev1", role::developer, "Developer 1", {"apps/web"}),
                  member("dev2", role::developer, "Developer 2", {"apps/api"}),
                  member("rev", role::reviewer, "Reviewer")});
}

} // namespace

class TeamStoreTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;

    QString journalPath() const { return directory_.filePath(QStringLiteral("journal.sqlite3")); }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        QFile::remove(journalPath());
        host_.conversations.clear();
        for (const QString& id : {"lead", "dev1", "dev2", "dev3", "dev4", "dev5", "dev6"})
            host_.add(id);
        host_.add(QStringLiteral("rev"), QStringLiteral("claude"));
        host_.add(QStringLiteral("rev2"), QStringLiteral("claude"));
        host_.add(QStringLiteral("elsewhere"), QStringLiteral("codex"), QStringLiteral("/other"));
    }

    void teamSurvivesRestartWithIdsAndOrder() {
        qint64 id = 0;
        {
            LocalJournal journal(journalPath());
            TeamCoordinator teams(&journal, &host_);
            teams.load();
            QSignalSpy changed(&teams, &TeamCoordinator::teamChanged);
            const auto result = teams.create(standard(), QStringLiteral("user"));
            QVERIFY2(result.ok(), qPrintable(result.error->message));
            id = result.teamId;
            // Creating, then the leader's first turn with the goal (#458).
            QVERIFY(changed.size() >= 1);
            QCOMPARE(changed.first().first().toJsonObject().value("members").toArray().size(), 4);
        }
        LocalJournal journal(journalPath());
        TeamCoordinator teams(&journal, &host_);
        teams.load();
        const Team* team = teams.team(id);
        QVERIFY(team);
        QCOMPARE(team->name, QStringLiteral("Checkout"));
        QCOMPARE(team->members.size(), 4);
        QCOMPARE(team->members.at(0).sessionId, QStringLiteral("lead"));
        QCOMPARE(team->members.at(1).paths, QStringList{"apps/web"});
        QCOMPARE(team->members.at(3).role, role::reviewer);
        QCOMPARE(teams.teamOf(QStringLiteral("dev2"))->id, id);
        // Clients see the leader first, then developers, then the reviewer.
        const QJsonArray members = teams.teamJson(*team).value("members").toArray();
        QCOMPARE(members.at(0).toObject().value("role").toString(), role::leader);
        QCOMPARE(members.at(3).toObject().value("role").toString(), role::reviewer);
        QCOMPARE(members.at(0).toObject().value("number").toString(), QStringLiteral("1000"));
    }

    void everyRuleIsRefusedWithItsCodeAndChangesNothing_data() {
        QTest::addColumn<int>("which");
        QTest::addColumn<QString>("code");
        QTest::newRow("second leader") << 0 << "role_taken";
        QTest::newRow("no leader") << 1 << "role_required";
        QTest::newRow("no reviewer") << 2 << "role_required";
        QTest::newRow("six developers") << 3 << "team_full";
        QTest::newRow("no developer") << 4 << "last_developer";
        QTest::newRow("other repository") << 5 << "other_repository";
        QTest::newRow("codex reviewer") << 6 << "reviewer_unsupported";
        QTest::newRow("overlapping lanes") << 7 << "lane_overlap";
        QTest::newRow("same conversation twice") << 8 << "already_member";
        QTest::newRow("second reviewer") << 9 << "role_taken";
        QTest::newRow("empty name") << 10 << "invalid_payload";
    }

    void everyRuleIsRefusedWithItsCodeAndChangesNothing() {
        QFETCH(int, which);
        QFETCH(QString, code);
        LocalJournal journal(journalPath());
        TeamCoordinator teams(&journal, &host_);
        teams.load();
        Team team = standard();
        switch (which) {
        case 0:
            team.members.append(member("dev3", role::leader, "Second lead"));
            break;
        case 1:
            team.members.removeFirst();
            break;
        case 2:
            team.members.removeLast();
            break;
        case 3:
            for (const QString& id : {"dev3", "dev4", "dev5", "dev6"})
                team.members.append(member(id, role::developer, id, {"lane/" + id}));
            break;
        case 4:
            team.members.removeAt(1);
            team.members.removeAt(1);
            break;
        case 5:
            team.members[1].sessionId = QStringLiteral("elsewhere");
            break;
        case 6:
            team.members[3].sessionId = QStringLiteral("dev3");
            break;
        case 7:
            team.members[2].paths = {QStringLiteral("apps/web/cart")};
            break;
        case 8:
            team.members[2].sessionId = QStringLiteral("dev1");
            break;
        case 9:
            team.members.append(member("rev2", role::reviewer, "Second reviewer"));
            break;
        case 10:
            team.members[1].name = QStringLiteral("  ");
            break;
        }
        const auto result = teams.create(team, QStringLiteral("user"));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.value_or(TeamError{}).code, code);
        QVERIFY(teams.teams().isEmpty());
        QVERIFY(journal.teamDocuments().isEmpty());
        QVERIFY(!journal.teamOfSession(QStringLiteral("lead")));
    }

    void membershipChangesFollowTheRules() {
        LocalJournal journal(journalPath());
        TeamCoordinator teams(&journal, &host_);
        teams.load();
        const qint64 id = teams.create(standard(), QStringLiteral("user")).teamId;
        QVERIFY(id > 0);
        // A conversation is on at most one team.
        Team other = draft({member("dev3", role::leader, "Lead"),
                            member("dev1", role::developer, "D", {"x"}),
                            member("rev2", role::reviewer, "R")});
        QCOMPARE(teams.create(other, "user").error.value_or(TeamError{}).code,
                 QStringLiteral("already_member"));
        // The leader and the reviewer stay; the team is deleted instead.
        QCOMPARE(teams.removeMember(id, "lead", "user").error.value_or(TeamError{}).code,
                 QStringLiteral("role_required"));
        QCOMPARE(teams.removeMember(id, "rev", "user").error.value_or(TeamError{}).code,
                 QStringLiteral("role_required"));
        QCOMPARE(teams.addMember(id, member("rev2", role::reviewer, "R2"), "user")
                     .error.value_or(TeamError{})
                     .code,
                 QStringLiteral("role_taken"));
        QCOMPARE(teams.addMember(id, member("elsewhere", role::developer, "E", {"z"}), "user")
                     .error.value_or(TeamError{})
                     .code,
                 QStringLiteral("other_repository"));
        QCOMPARE(teams.addMember(id, member("dev3", role::developer, "D3", {"apps/api/v2"}), "user")
                     .error.value_or(TeamError{})
                     .code,
                 QStringLiteral("lane_overlap"));
        for (const QString& next : {"dev3", "dev4", "dev5"})
            QVERIFY(teams
                        .addMember(id, member(next, role::developer, next, {QString("l/") + next}),
                                   "user")
                        .ok());
        QCOMPARE(teams.addMember(id, member("dev6", role::developer, "D6", {"l6"}), "user")
                     .error.value_or(TeamError{})
                     .code,
                 QStringLiteral("team_full"));
        QCOMPARE(teams.updateMember(id, "dev1", "Web", {"apps/api"}, "user")
                     .error.value_or(TeamError{})
                     .code,
                 QStringLiteral("lane_overlap"));
        QVERIFY(teams.updateMember(id, "dev1", "Web", {"apps/web", "packages/ui"}, "user").ok());
        QCOMPARE(teams.team(id)->member("dev1")->paths, (QStringList{"apps/web", "packages/ui"}));
        for (const QString& gone : {"dev2", "dev3", "dev4", "dev5"})
            QVERIFY(teams.removeMember(id, gone, "user").ok());
        QCOMPARE(teams.removeMember(id, "dev1", "user").error.value_or(TeamError{}).code,
                 QStringLiteral("last_developer"));
        // A removed developer can join another team and is archived freely.
        QVERIFY(!teams.isMember("dev2"));
        QVERIFY(!teams.archiveRefusal("dev2"));
        QCOMPARE(teams.archiveRefusal("dev1")->code, QStringLiteral("team_member"));
        QVERIFY(!journal.teamUpdates(id, false, 100).isEmpty());
    }

    void deletedTeamLeavesConversationsAndItsIdIsNeverReused() {
        LocalJournal journal(journalPath());
        TeamCoordinator teams(&journal, &host_);
        teams.load();
        const qint64 first = teams.create(standard(), "user").teamId;
        QSignalSpy removed(&teams, &TeamCoordinator::teamRemoved);
        QVERIFY(!teams.remove(first));
        QCOMPARE(removed.size(), 1);
        QVERIFY(teams.teams().isEmpty());
        QVERIFY(journal.teamUpdates(first, false, 10).isEmpty());
        QVERIFY(!teams.isMember("lead"));
        QVERIFY(host_.teamConversation("lead"));
        const qint64 second = teams.create(standard(), "user").teamId;
        QVERIFY(second > first);
    }

    void deletedConversationsLeaveTheirTeam() {
        LocalJournal journal(journalPath());
        TeamCoordinator teams(&journal, &host_);
        teams.load();
        const qint64 id = teams.create(standard(), "user").teamId;
        teams.conversationDeleted("dev2");
        QCOMPARE(teams.team(id)->count(role::developer), 1);
        QVERIFY(!teams.team(id)->paused);
        // The leader's deletion pauses the team and names the empty role.
        teams.conversationDeleted("lead");
        const Team* team = teams.team(id);
        QVERIFY(team->paused);
        QCOMPARE(team->pausedReason, QStringLiteral("empty_role"));
        QCOMPARE(team->openDecisions(), 1);
        QCOMPARE(team->decisions.first().kind, QStringLiteral("empty_role"));
        QVERIFY(team->decisions.first().question.contains("leader"));
        // Filling the role lifts the pause and withdraws the decision.
        QVERIFY(teams.addMember(id, member("dev3", role::leader, "New lead"), "user").ok());
        QVERIFY(!teams.team(id)->paused);
        QCOMPARE(teams.team(id)->openDecisions(), 0);
    }

    void journalWithoutTeamsOpensAndGetsTheTables() {
        {
            LocalJournal journal(journalPath());
            QVERIFY(journal.isOpen());
        }
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", "older-journal");
            database.setDatabaseName(journalPath());
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec("DROP TABLE teams"));
            QVERIFY(query.exec("DROP TABLE team_members"));
            QVERIFY(query.exec("DROP TABLE team_updates"));
            database.close();
        }
        QSqlDatabase::removeDatabase("older-journal");
        LocalJournal journal(journalPath());
        QVERIFY(journal.isOpen());
        TeamCoordinator teams(&journal, &host_);
        teams.load();
        QVERIFY(teams.teams().isEmpty());
        QVERIFY(teams.create(standard(), "user").ok());
    }

    // A journal that cannot be read is not a journal without teams: the coordinator says it
    // has not loaded, keeps what it knew, and reads again when asked.
    void anUnreadableJournalIsNotAnEmptyOne() {
        LocalJournal journal(journalPath());
        TeamCoordinator teams(&journal, &host_);
        teams.load();
        QVERIFY(teams.isLoaded());
        QVERIFY(teams.create(standard(), "user").ok());
        const auto sql = [this](const QString& statement) {
            bool ran = false;
            {
                auto database = QSqlDatabase::addDatabase("QSQLITE", "break-journal");
                database.setDatabaseName(journalPath());
                ran = database.open() && QSqlQuery(database).exec(statement);
                database.close();
            }
            QSqlDatabase::removeDatabase("break-journal");
            return ran;
        };
        QVERIFY(sql("ALTER TABLE teams RENAME TO teams_away"));
        QTest::ignoreMessage(QtWarningMsg, "Unable to read the teams from the journal.");
        teams.load();
        QVERIFY(!teams.isLoaded());
        QCOMPARE(teams.teams().size(), 1);
        QVERIFY(sql("ALTER TABLE teams_away RENAME TO teams"));
        teams.load();
        QVERIFY(teams.isLoaded());
        QCOMPARE(teams.teams().size(), 1);
    }
};

QTEST_GUILESS_MAIN(TeamStoreTest)
#include "TeamStoreTest.moc"
