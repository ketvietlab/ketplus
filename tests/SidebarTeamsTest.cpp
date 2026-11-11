// ADR 0114 and #464: a team is a folder inside its repository in the sidebar. The order of the
// rows, the folder and conversation menus with their reasons, joining by drag and the missing
// feature, against a fake daemon. Run with QT_QPA_PLATFORM=offscreen.
#include "KetBadge.h"
#include "KetButton.h"
#include "KetConversationRow.h"
#include "KetStyle.h"
#include "KetTreeItem.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamMenus.h"
#include "workspace/SidebarRepositoryList.h"

#include <QAction>
#include <QApplication>
#include <QDragEnterEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLayout>
#include <QMenu>
#include <QMimeData>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTimer>
#include <QtTest>

using namespace ketplus;

namespace {

QJsonObject member(const QString& session, const QString& role, const QString& name) {
    return {{"session_id", session}, {"role", role}, {"name", name}};
}

QJsonObject team(const QString& id, int developers, int revision = 1) {
    QJsonArray members{member("s-lead", "leader", "Lead")};
    for (int index = 1; index <= developers; ++index)
        members.append(member(QStringLiteral("s-dev%1").arg(index), "developer",
                              QStringLiteral("Dev %1").arg(index)));
    members.append(member("s-rev", "reviewer", "Review"));
    return {{"id", id},           {"name", "Shop"},       {"repository", "/repo"},
            {"members", members}, {"revision", revision}, {"open_decisions", 0}};
}

QVariantMap conversation(const QString& id, const QString& session, const QString& title,
                         const QString& role = {}) {
    QVariantMap row{
        {"id", id}, {"sessionId", session}, {"title", title}, {"path", "/repo"}, {"started", true}};
    if (!role.isEmpty()) {
        row.insert("teamRole", role);
        row.insert("tags", QVariantList{QVariantMap{{"label", team_roles::label(role)},
                                                    {"color", team_roles::color(role).name()},
                                                    {"locked", true}}});
    }
    return row;
}

SidebarRepository repository(const QString& path, int openDecisions = 0, bool full = false,
                             const QString& teamId = "7") {
    SidebarRepository result{path, "repo", true,
                             QVariantList{conversation("c-other", "s-other", "Other")}};
    result.teams.append({.id = teamId,
                         .name = "Shop",
                         .openDecisions = openDecisions,
                         .full = full,
                         .members = {conversation("c-lead", "s-lead", "Lead", "leader"),
                                     conversation("c-dev1", "s-dev1", "Dev 1", "developer"),
                                     conversation("c-rev", "s-rev", "Review", "reviewer")}});
    return result;
}

QStringList texts(const QMenu& menu) {
    QStringList result;
    for (const auto* action : menu.actions())
        result.append(action->isSeparator() ? QStringLiteral("—") : action->text());
    return result;
}

QString reason(const QAction* action) {
    return action->property(ket::Style::kDescriptionProperty).toString();
}

QMimeData* dragged(const QString& session, const QString& repository) {
    auto* data = new QMimeData;
    data->setData(
        SidebarRepositoryList::kConversationMimeType,
        QJsonDocument(QJsonObject{{"session_id", session}, {"repository", repository}}).toJson());
    return data;
}

// The fake daemon: answers team requests from a script and records them.
struct FakeDaemon {
    QList<QPair<QString, QJsonObject>> sent;
    QHash<QString, QJsonObject> answers;
    TeamDirectory::Transport transport() {
        return [this](const QString& type, const QJsonObject& payload, TeamDirectory::Reply reply) {
            sent.append({type, payload});
            const QJsonObject answer = answers.value(type, {{"ok", true}});
            reply(answer, answer.value("ok").toBool() ? QString{}
                                                      : answer.value("error_message").toString());
        };
    }
};

} // namespace

class SidebarTeamsTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
    }

    // Its teams first, then its other conversations; leader, developers, reviewer; the role
    // is each member's only tag, locked.
    void aTeamIsAFolderBeforeTheRepositorysConversations() {
        SidebarRepositoryList list;
        list.setRepositories({repository("/repo")});
        list.resize(280, 600);
        auto* group = list.findChild<QWidget*>("productRepositoryGroup");
        QVERIFY(group);
        auto* folder = group->findChild<QWidget*>("productTeamFolder", Qt::FindDirectChildrenOnly);
        QVERIFY(folder);
        QCOMPARE(folder->property("teamId").toString(), QStringLiteral("7"));
        const auto layout = group->layout();
        QCOMPARE(layout->indexOf(folder), 1); // right after the repository's header
        QStringList members;
        for (auto* row : folder->findChildren<ket::ConversationRow*>("productConversationButton",
                                                                     Qt::FindDirectChildrenOnly)) {
            members.append(row->property("teamRole").toString());
            QCOMPARE(row->tags().size(), 1);
            QVERIFY(row->tags().first().locked);
            QCOMPARE(row->tags().first().label, team_roles::label(members.last()));
        }
        QCOMPARE(members, (QStringList{"leader", "developer", "reviewer"}));
        const auto others = group->findChildren<ket::ConversationRow*>("productConversationButton",
                                                                       Qt::FindDirectChildrenOnly);
        QCOMPARE(others.size(), 1);
        QVERIFY(layout->indexOf(others.first()) > layout->indexOf(folder));
    }

    // The count is the open decisions; the name opens the team, the icon folds it.
    void theFolderCountsDecisionsOpensAndFolds() {
        SidebarRepositoryList list;
        list.setRepositories({repository("/repo", 2)});
        list.show();
        auto* folder = list.findChild<QWidget*>("productTeamFolder");
        auto* count = folder->findChild<ket::Badge*>("productTeamDecisionCount");
        QVERIFY(count->isVisibleTo(&list));
        QCOMPARE(count->text(), QStringLiteral("2"));
        QCOMPARE(count->accessibleName(), QStringLiteral("2 decisions wait for you"));
        QSignalSpy opened(&list, &SidebarRepositoryList::teamRequested);
        folder->findChild<QAbstractButton*>("productTeamFolderName")->click();
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.first().first().toString(), QStringLiteral("7"));
        // The update in place lowers the count without a rebuild.
        list.setRepositories({repository("/repo", 0)});
        QCOMPARE(list.findChild<QWidget*>("productTeamFolder"), folder);
        QVERIFY(!count->isVisibleTo(&list));
        auto* lead =
            folder->findChildren<ket::ConversationRow*>("productConversationButton").first();
        QVERIFY(lead->isVisibleTo(&list));
        folder->findChild<QAbstractButton*>("productTeamFolderFold")->click();
        QVERIFY(!lead->isVisibleTo(&list));
        folder->findChild<QAbstractButton*>("productTeamFolderFold")->click();
        QVERIFY(lead->isVisibleTo(&list));
        QSignalSpy menu(&list, &SidebarRepositoryList::teamMenuRequested);
        folder->findChild<ket::GroupHeader*>("productTeamFolderHeader")->action()->click();
        QCOMPARE(menu.size(), 1);
    }

    void theFolderMenuSaysWhyAFullTeamTakesNoMember() {
        QMenu menu;
        auto actions = addTeamFolderMenuActions(menu, team("7", 2));
        QCOMPARE(texts(menu), (QStringList{"Open team", "Add member…", "—", "Delete team…"}));
        QVERIFY(actions.addMember->isEnabled());
        QVERIFY(actions.remove->property(ket::Style::kDangerProperty).toBool());
        QMenu full;
        actions = addTeamFolderMenuActions(full, team("7", 5));
        QVERIFY(!actions.addMember->isEnabled());
        QCOMPARE(reason(actions.addMember), QStringLiteral("A team has at most five developers."));
    }

    void theRepositoryAddsAConversationOrATeam() {
        QMenu menu;
        addRepositoryAddMenuActions(menu);
        QCOMPARE(texts(menu), (QStringList{"New conversation", "New team…"}));
        // Without teams/1 the + starts a conversation at once, as before.
        SidebarRepositoryList list;
        list.setRepositories({repository("/repo")});
        QSignalSpy created(&list, &SidebarRepositoryList::newConversationRequested);
        list.findChild<QAbstractButton*>("productNewConversationButton")->click();
        QCOMPARE(created.size(), 1);
    }

    // The menu has closed before New team… opens its dialog, whose backdrop is a snapshot of
    // the screen: opened at once, the snapshot kept the menu on screen.
    void newTeamOpensOnceTheMenuHasClosed() {
        SidebarRepositoryList list;
        list.setTeamsAvailable(true);
        list.setRepositories({repository("/repo")});
        list.show();
        QSignalSpy requested(&list, &SidebarRepositoryList::newTeamRequested);
        bool menuVisibleOnRequest = false;
        connect(&list, &SidebarRepositoryList::newTeamRequested, &list, [&menuVisibleOnRequest] {
            menuVisibleOnRequest = QApplication::activePopupWidget() != nullptr;
        });
        QTimer::singleShot(0, &list, [] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            QVERIFY(menu);
            menu->setActiveAction(menu->findChild<QAction*>("newTeamAction"));
            QTest::keyClick(menu, Qt::Key_Return);
        });
        list.findChild<QAbstractButton*>("productNewConversationButton")->click();
        QCOMPARE(requested.size(), 0);
        QTRY_COMPARE(requested.size(), 1);
        QCOMPARE(requested.at(0).at(0).toString(), QStringLiteral("/repo"));
        QVERIFY(!menuVisibleOnRequest);
    }

    void theConversationMenuJoinsAndLeavesWithItsReasons() {
        const QList<QJsonObject> teams{team("7", 1), team("8", 5)};
        QMenu outside;
        auto actions = addConversationTeamActions(outside, teams, "s-other");
        QCOMPARE(actions.join.size(), 2);
        QCOMPARE(actions.join.at(0)->text(), QStringLiteral("Add to Shop"));
        QVERIFY(actions.join.at(0)->isEnabled());
        QVERIFY(!actions.join.at(1)->isEnabled());
        QCOMPARE(reason(actions.join.at(1)), QStringLiteral("Shop has five developers."));
        QVERIFY(!actions.leave);

        const auto leave = [&](const QJsonObject& on, const QString& session) {
            QMenu menu;
            const auto result = addConversationTeamActions(menu, {on}, session);
            return std::pair{result.leave->isEnabled(), reason(result.leave)};
        };
        QCOMPARE(leave(team("7", 2), "s-lead").second,
                 QStringLiteral("The leader leads the team. Delete the team instead."));
        QCOMPARE(
            leave(team("7", 2), "s-rev").second,
            QStringLiteral("The team's reviewer reviews every task. Delete the team instead."));
        QCOMPARE(leave(team("7", 1), "s-dev1").second,
                 QStringLiteral("A team keeps at least one developer."));
        QVERIFY(leave(team("7", 2), "s-dev2").first);
        // No team in the repository: nothing is added, not even a separator.
        QMenu none;
        addConversationTeamActions(none, {}, "s-other");
        QVERIFY(none.actions().isEmpty());
    }

    // A drop on a folder of its own repository joins; another repository, a full team or a
    // conversation already on a team is not accepted.
    void aDropJoinsOnlyWhereItCan() {
        SidebarRepositoryList list;
        list.setRepositories({repository("/repo"), repository("/other", 0, false, "8")});
        list.show();
        QWidget* folder = nullptr;
        for (auto* candidate : list.findChildren<QWidget*>("productTeamFolder"))
            if (candidate->property("repositoryPath").toString() == "/repo")
                folder = candidate;
        QVERIFY(folder);
        QSignalSpy joined(&list, &SidebarRepositoryList::teamJoinRequested);
        const auto enter = [&](QMimeData* data) {
            QDragEnterEvent event(QPoint(4, 4), Qt::MoveAction, data, Qt::LeftButton,
                                  Qt::NoModifier);
            QCoreApplication::sendEvent(folder, &event);
            const bool accepted = event.isAccepted();
            delete data;
            return accepted;
        };
        QVERIFY(enter(dragged("s-other", "/repo")));
        QVERIFY(!enter(dragged("s-other", "/other")));
        QVERIFY(!enter(dragged("s-dev1", "/repo")));
        std::unique_ptr<QMimeData> data(dragged("s-other", "/repo"));
        QDropEvent drop(QPointF(4, 4), Qt::MoveAction, data.get(), Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(folder, &drop);
        QCOMPARE(joined.size(), 1);
        QCOMPARE(joined.first().at(0).toString(), QStringLiteral("7"));
        QCOMPARE(joined.first().at(1).toString(), QStringLiteral("s-other"));
        list.setRepositories({repository("/repo", 0, true)});
        folder = list.findChild<QWidget*>("productTeamFolder");
        QVERIFY(!enter(dragged("s-other", "/repo")));
        QVERIFY(!list.acceptsDrop("7", "/repo", "s-other"));
    }

    // The directory reads the teams once, then follows the daemon's events; without
    // teams/1 it asks nothing and has nothing.
    void theDirectoryFollowsTheDaemon() {
        FakeDaemon daemon;
        daemon.answers.insert("team.list", {{"ok", true}, {"teams", QJsonArray{team("7", 1)}}});
        TeamDirectory teams;
        teams.setTransport(daemon.transport());
        teams.refresh();
        QVERIFY(daemon.sent.isEmpty());
        QVERIFY(teams.teams().isEmpty());
        QSignalSpy changed(&teams, &TeamDirectory::changed);
        teams.setAvailable(true);
        QCOMPARE(daemon.sent.size(), 1);
        QCOMPARE(daemon.sent.first().first, QStringLiteral("team.list"));
        QCOMPARE(teams.teams("/repo").size(), 1);
        QVERIFY(teams.teamOf("s-lead"));
        QVERIFY(!teams.teamOf("s-other"));
        // A team created on another client.
        teams.applyEvent("team.changed", {{"team", team("9", 2)}});
        QCOMPARE(teams.teams().size(), 2);
        // An older revision does not undo a newer one.
        teams.applyEvent("team.changed", {{"team", team("9", 3, 5)}});
        teams.applyEvent("team.changed", {{"team", team("9", 1, 4)}});
        QCOMPARE(TeamDirectory::count(*teams.team("9"), "developer"), 3);
        teams.applyEvent("team.removed", {{"team_id", "9"}});
        QVERIFY(!teams.team("9"));
        // A join answers with the team, which updates the cache at once.
        QJsonObject joined = team("7", 1, 2);
        QJsonArray members = joined.value("members").toArray();
        members.append(member("s-other", "developer", "Other"));
        joined.insert("members", members);
        daemon.answers.insert("team.member.join", {{"ok", true}, {"team", joined}});
        QString error = QStringLiteral("unset");
        teams.request("team.member.join", {{"team_id", "7"}, {"session_id", "s-other"}},
                      [&](const QJsonObject&, const QString& failure) { error = failure; });
        QVERIFY(error.isEmpty());
        QCOMPARE(teams.teamOf("s-other")->value("id").toString(), QStringLiteral("7"));
        QCOMPARE(daemon.sent.last().second.value("session_id").toString(),
                 QStringLiteral("s-other"));
        // A refusal changes nothing and says why.
        daemon.answers.insert("team.member.remove",
                              {{"ok", false}, {"error_message", "The leader leads the team."}});
        teams.request("team.member.remove", {{"team_id", "7"}, {"session_id", "s-lead"}},
                      [&](const QJsonObject&, const QString& failure) { error = failure; });
        QCOMPARE(error, QStringLiteral("The leader leads the team."));
        QVERIFY(teams.teamOf("s-lead"));
        teams.setAvailable(false);
        QVERIFY(teams.teams().isEmpty());
        QVERIFY(changed.size() >= 4);
        teams.request("team.list", {},
                      [&](const QJsonObject&, const QString& failure) { error = failure; });
        QVERIFY(!error.isEmpty());
    }

    // ADR 0115: a daemon that goes away leaves its teams, stale, so a team's members stay in
    // one folder while it reconnects; the list is read again once it is back.
    void aDisconnectKeepsTheTeamsUntilTheDaemonIsBack() {
        FakeDaemon daemon;
        daemon.answers.insert("team.list", {{"ok", true}, {"teams", QJsonArray{team("7", 2)}}});
        TeamDirectory teams;
        teams.setTransport(daemon.transport());
        teams.setAvailable(true);
        QVERIFY(teams.teamOf("s-dev2"));
        QSignalSpy changed(&teams, &TeamDirectory::changed);
        teams.setDisconnected();
        QVERIFY(!teams.isAvailable());
        QVERIFY(teams.isStale());
        QVERIFY(teams.hasTeams());
        QCOMPARE(changed.size(), 1);
        QCOMPARE(teams.teamOf("s-dev2")->value("id").toString(), QStringLiteral("7"));
        // Nothing is asked of a daemon that is not there.
        const auto asked = daemon.sent.size();
        teams.refresh();
        QString error;
        teams.request("team.get", {{"team_id", "7"}},
                      [&](const QJsonObject&, const QString& failure) { error = failure; });
        QVERIFY(!error.isEmpty());
        QCOMPARE(daemon.sent.size(), asked);
        // Back: read again, and no longer stale.
        teams.setAvailable(true);
        QCOMPARE(daemon.sent.size(), asked + 1);
        QCOMPARE(daemon.sent.last().first, QStringLiteral("team.list"));
        QVERIFY(!teams.isStale());
        QVERIFY(teams.teamOf("s-dev2"));
        // A daemon without teams/1 has none.
        teams.setDisconnected();
        teams.setAvailable(false);
        QVERIFY(!teams.hasTeams());
        QVERIFY(teams.teams().isEmpty());
    }

    // team.list and team.changed carry no tasks: the ones a page read stay. A team too large
    // for one message arrives as its id, and the list is read again.
    void directoryEventsKeepTheTasksAPageRead() {
        FakeDaemon daemon;
        daemon.answers.insert("team.list", {{"ok", true}, {"teams", QJsonArray{team("7", 1)}}});
        TeamDirectory teams;
        teams.setTransport(daemon.transport());
        teams.setAvailable(true);
        QJsonObject read = team("7", 1, 2);
        read.insert("tasks", QJsonArray{QJsonObject{{"id", "T-1"}}});
        read.insert("decisions", QJsonArray{QJsonObject{{"id", "D-1"}}});
        teams.applyEvent("team.changed", {{"team", read}});
        QJsonObject directory = team("7", 2, 3);
        directory.insert("directory", true);
        directory.insert("name", "Renamed");
        teams.applyEvent("team.changed", {{"team", directory}});
        const QJsonObject known = *teams.team("7");
        QCOMPARE(known.value("name").toString(), QStringLiteral("Renamed"));
        QCOMPARE(known.value("revision").toInt(), 3);
        QCOMPARE(known.value("tasks").toArray().size(), 1);
        QCOMPARE(known.value("decisions").toArray().size(), 1);
        QVERIFY(teams.teamOf("s-dev2"));
        const auto asked = daemon.sent.size();
        teams.applyEvent("team.changed", {{"truncated", true}, {"team_id", "7"}, {"revision", 4}});
        QCOMPARE(daemon.sent.size(), asked + 1);
        QCOMPARE(daemon.sent.last().first, QStringLiteral("team.list"));
    }

    // The daemon may still be starting when the window asks: a list that failed is asked
    // for again, or the sidebar would show a team's members as loose conversations.
    void aTeamListThatFailedIsAskedForAgain() {
        FakeDaemon daemon;
        TeamDirectory teams;
        teams.setTransport(daemon.transport());
        teams.setFirstRetryForTesting(20);
        daemon.answers.insert("team.list", {{"ok", false}, {"error_message", "Starting"}});
        QSignalSpy changed(&teams, &TeamDirectory::changed);
        teams.setAvailable(true);
        QVERIFY(teams.teams().isEmpty());
        QTRY_VERIFY(std::count_if(daemon.sent.cbegin(), daemon.sent.cend(),
                                  [](const auto& call) { return call.first == "team.list"; }) >= 3);
        daemon.answers.insert(
            "team.list",
            {{"ok", true},
             {"teams", QJsonArray{QJsonObject{{"id", "7"}, {"name", "Shop"}, {"revision", 1}}}}});
        QTRY_COMPARE(teams.teams().size(), 1);
        QVERIFY(!changed.isEmpty());
        // Once it has the list it stops asking.
        const auto asked = daemon.sent.size();
        QTest::qWait(120);
        QCOMPARE(daemon.sent.size(), asked);
        // A daemon that went away is not asked.
        daemon.answers.insert("team.list", {{"ok", false}, {"error_message", "Starting"}});
        teams.refresh();
        teams.setAvailable(false);
        const auto after = daemon.sent.size();
        QTest::qWait(120);
        QCOMPARE(daemon.sent.size(), after);
    }
};

QTEST_MAIN(SidebarTeamsTest)
#include "SidebarTeamsTest.moc"
