// ADR 0114 and #464: a team is a folder inside its repository in the sidebar. The order of the
// rows, the folder and conversation menus with their reasons, joining by drag and the missing
// feature, against a fake daemon. Run with QT_QPA_PLATFORM=offscreen.
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetProfile.h"
#include "KetSidebarTree.h"
#include "KetStyle.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamMenus.h"
#include "ui/InterfaceZoom.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"
#include "workspace/SidebarRepositoryList.h"

#include <QAction>
#include <QApplication>
#include <QDragEnterEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLayout>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
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

QVector<ket::SidebarRow*> rowsIn(QWidget* parent) {
    QVector<ket::SidebarRow*> rows;
    for (auto* row : parent->findChildren<ket::SidebarRow*>("productConversationButton"))
        rows.append(row);
    return rows;
}

// A conversation of no team: the rows that are not members.
QVector<ket::SidebarRow*> looseRows(QWidget* parent) {
    QVector<ket::SidebarRow*> rows;
    for (auto* row : rowsIn(parent))
        if (!row->property("teamMember").toBool())
            rows.append(row);
    return rows;
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

    void init() {
        QSettings().remove(QStringLiteral("sidebar"));
        ket::Appearance::instance().setZoom(1.0);
    }

    // A turn starting or ending changes what the rows are built from, so the tree is built again.
    // The new tree is on screen in the same pass: a group left to the layout's queued show drew an
    // empty sidebar until the next event loop turn, and running agents made it flicker.
    void aRebuiltTreeIsShownAtOnce() {
        QWidget window;
        auto* list = new SidebarRepositoryList(&window);
        list->setRepositories({repository("/repo")});
        window.resize(280, 600);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();
        const QVector<ket::SidebarRow*> before = rowsIn(list);
        QVERIFY(!before.isEmpty());
        const QPointer<ket::SidebarRow> old = before.first();

        SidebarRepository running = repository("/repo");
        QVariantMap other = running.conversations.first().toMap();
        other.insert("running", true);
        other.insert("agentTurnNumber", "12");
        running.conversations[0] = other;
        list->setRepositories({running});

        // No event processed since: the rebuilt tree must already be visible and laid out.
        const QVector<ket::SidebarRow*> after = rowsIn(list);
        QVERIFY(old.isNull());
        QCOMPARE(after.size(), before.size());
        for (auto* row : after) {
            QVERIFY2(row->isVisible(), qPrintable(row->accessibleName()));
        }
        auto* header = list->findChild<ket::SidebarRepositoryHeader*>(
            QStringLiteral("productRepositoryGroupButton"));
        QVERIFY(header != nullptr && header->isVisible());
        QVERIFY(header->height() > 0);
        QVERIFY(list->layoutProblems().isEmpty());
    }

    // The repository is the root; its teams are branches directly under it, then the
    // conversations of no team. Leader, developers, reviewer; the role is in the hint and the
    // accessible description, never a tag.
    void aTeamIsABranchBeforeTheRepositorysConversations() {
        SidebarRepositoryList list;
        list.setRepositories({repository("/repo")});
        list.resize(280, 600);
        auto* group = list.findChild<QWidget*>("productRepositoryGroup");
        QVERIFY(group);
        QVERIFY(group->findChild<ket::SidebarRepositoryHeader*>("productRepositoryGroupButton"));
        auto* held = group->findChild<ket::SidebarTreeGuide*>("productRepositoryHeld");
        QVERIFY(held);
        QCOMPARE(held->level(), 0);
        auto* folder = held->findChild<QWidget*>("productTeamFolder", Qt::FindDirectChildrenOnly);
        QVERIFY(folder);
        QCOMPARE(folder->property("teamId").toString(), QStringLiteral("7"));
        QCOMPARE(held->partsLayout()->indexOf(folder), 0); // first under the repository
        QStringList members;
        for (auto* row : rowsIn(folder)) {
            members.append(row->property("teamRole").toString());
            QCOMPARE(row->level(), 1);
            QVERIFY(row->accessibleDescription().contains(team_roles::label(members.last())));
            QVERIFY(row->toolTip().contains(team_roles::label(members.last())));
        }
        QCOMPARE(members, (QStringList{"leader", "developer", "reviewer"}));
        const auto others = looseRows(group);
        QCOMPARE(others.size(), 1);
        QCOMPARE(others.first()->level(), 0);
        QVERIFY(held->partsLayout()->indexOf(others.first()) >
                held->partsLayout()->indexOf(folder));
    }

    // The branch shows the open decisions as its mark and a semibold name; the name opens the
    // team, the chevron folds it.
    void theBranchShowsDecisionsOpensAndFolds() {
        SidebarRepositoryList list;
        list.setRepositories({repository("/repo", 2)});
        list.show();
        auto* folder = list.findChild<QWidget*>("productTeamFolder");
        auto* branch = folder->findChild<ket::SidebarTeamBranch*>("productTeamFolderHeader");
        QVERIFY(branch);
        QCOMPARE(branch->decisionCount(), 2);
        QCOMPARE(branch->memberCount(), 3);
        QSignalSpy opened(&list, &SidebarRepositoryList::teamRequested);
        folder->findChild<QAbstractButton*>("productTeamFolderName")->click();
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.first().first().toString(), QStringLiteral("7"));
        // The update in place lowers the count without a rebuild.
        list.setRepositories({repository("/repo", 0)});
        QCOMPARE(list.findChild<QWidget*>("productTeamFolder"), folder);
        QCOMPARE(branch->decisionCount(), 0);
        auto* lead = rowsIn(folder).first();
        QVERIFY(lead->isVisibleTo(&list));
        folder->findChild<QAbstractButton*>("productTeamFolderFold")->click();
        QVERIFY(!lead->isVisibleTo(&list));
        QVERIFY(!branch->isExpanded());
        folder->findChild<QAbstractButton*>("productTeamFolderFold")->click();
        QVERIFY(lead->isVisibleTo(&list));
        QSignalSpy menu(&list, &SidebarRepositoryList::teamMenuRequested);
        branch->menuAction()->click();
        QCOMPARE(menu.size(), 1);
    }

    // "Conversations (n)" labels the conversations of no team, only where the repository has
    // teams; its + starts a conversation. "Show N more" says how many it adds.
    void theSectionLabelAndShowMoreSayWhatTheyHold() {
        SidebarRepository plain{"/plain", "plain", false, {}};
        for (int index = 0; index < 7; ++index)
            plain.conversations.append(conversation(QStringLiteral("p%1").arg(index),
                                                    QStringLiteral("sp%1").arg(index),
                                                    QStringLiteral("Plain %1").arg(index)));
        SidebarRepositoryList list;
        list.setRepositories({repository("/repo"), plain});
        list.show();
        QWidget* teamGroup = nullptr;
        QWidget* plainGroup = nullptr;
        for (auto* group : list.findChildren<QWidget*>("productRepositoryGroup"))
            (group->property("repositoryPath") == "/repo" ? teamGroup : plainGroup) = group;
        auto* section =
            teamGroup->findChild<ket::SidebarSectionLabel*>("productConversationsSection");
        QVERIFY(section && section->isVisibleTo(&list));
        QCOMPARE(section->displayText(), QStringLiteral("Conversations (1)"));
        QVERIFY(!plainGroup->findChild<ket::SidebarSectionLabel*>());
        QSignalSpy created(&list, &SidebarRepositoryList::newConversationRequested);
        section->action()->click();
        QCOMPARE(created.size(), 1);
        QCOMPARE(created.first().first().toString(), QStringLiteral("/repo"));

        auto* more = plainGroup->findChild<QPushButton*>("productShowMoreConversations");
        QVERIFY(more && more->isVisibleTo(&list));
        QCOMPARE(more->text(), QStringLiteral("Show 2 more"));
        int shown = 0;
        for (auto* row : looseRows(plainGroup))
            shown += row->isVisibleTo(&list) ? 1 : 0;
        QCOMPARE(shown, 5);
        more->click();
        QVERIFY(!more->isVisibleTo(&list));
        for (auto* row : looseRows(plainGroup))
            QVERIFY(row->isVisibleTo(&list));
    }

    // A repository and a team stay folded across a restart; a folded repository shows one dot
    // while something inside waits.
    void foldsSurviveARestartAndAFoldedRepositoryShowsWhatWaits() {
        {
            SidebarRepositoryList list;
            list.setRepositories({repository("/repo")});
            list.show();
            list.findChild<QWidget*>("productTeamFolder")
                ->findChild<QAbstractButton*>("productTeamFolderFold")
                ->click();
            list.findChild<ket::SidebarRepositoryHeader*>("productRepositoryGroupButton")->click();
        }
        SidebarRepositoryList list;
        list.setRepositories({repository("/repo", 1)});
        list.show();
        auto* header =
            list.findChild<ket::SidebarRepositoryHeader*>("productRepositoryGroupButton");
        QVERIFY(!header->isExpanded());
        QVERIFY(!list.findChild<QWidget*>("productRepositoryHeld")->isVisibleTo(&list));
        QCOMPARE(header->conversationCount(), 4);
        QCOMPARE(header->waitingCount(), 5); // four finished and unread, one decision
        QVERIFY(header->isWaitingDotShown());
        header->click();
        QVERIFY(header->isExpanded());
        auto* branch = list.findChild<ket::SidebarTeamBranch*>("productTeamFolderHeader");
        QVERIFY(!branch->isExpanded());
        QVERIFY(!rowsIn(list.findChild<QWidget*>("productTeamFolder")).first()->isVisibleTo(&list));
        QVERIFY(!header->isWaitingDotShown());
    }

    // A team's fold belongs to its repository: another repository's team with the same id keeps
    // its own. A team that left a repository forgets its fold; a repository whose teams have not
    // arrived yet keeps them.
    void aTeamsFoldIsItsRepositorysAndForgetsRemovedTeams() {
        const QString setting = QStringLiteral("sidebar/collapsedTeams");
        SidebarRepositoryList list;
        list.setTeamsAvailable(true);
        list.setRepositories({repository("/one"), repository("/two")});
        list.show();
        const auto folders = list.findChildren<QWidget*>("productTeamFolder");
        QCOMPARE(folders.size(), 2);
        QWidget* first = folders.first()->property("repositoryPath") == "/one" ? folders.first()
                                                                               : folders.last();
        QWidget* second = first == folders.first() ? folders.last() : folders.first();
        first->findChild<QAbstractButton*>("productTeamFolderFold")->click();
        QVERIFY(!first->findChild<ket::SidebarTeamBranch*>()->isExpanded());
        QVERIFY(second->findChild<ket::SidebarTeamBranch*>()->isExpanded());
        QCOMPARE(QSettings().value(setting).toStringList(), QStringList{"/one\n7"});

        // /two lists no team yet: its stored folds stay.
        QSettings().setValue(setting, QStringList{"/one\n7", "/two\n7", "legacy-id"});
        SidebarRepository loading = repository("/two");
        loading.teams.clear();
        SidebarRepositoryList restarted;
        restarted.setTeamsAvailable(true);
        restarted.setRepositories({repository("/one", 0, false, "8"), loading});
        // Team 7 left /one; /two keeps its fold; an id stored alone cannot be placed.
        QCOMPARE(QSettings().value(setting).toStringList(), QStringList{"/two\n7"});
    }

    // A row is the state's mark, the title and #number; finished and unread is a dot. The state
    // in words and the worktree are in the hint and the description.
    void aRowSaysItsStateWithoutTags() {
        SidebarRepository plain{"/plain", "plain", true, {}};
        QVariantMap done = conversation("c1", "s1", "Done");
        done.insert("number", "42");
        done.insert("path", "/work/plain-feature");
        QVariantMap asking = conversation("c2", "s2", "Asking");
        asking.insert("needsInput", true);
        plain.conversations = {done, asking};
        SidebarRepositoryList list;
        list.setRepositories({plain});
        list.show();
        const auto rows = looseRows(&list);
        QCOMPARE(rows.size(), 2);
        ket::SidebarRow* unread = rows.at(0)->text() == "Done" ? rows.at(0) : rows.at(1);
        ket::SidebarRow* waiting = unread == rows.at(0) ? rows.at(1) : rows.at(0);
        QVERIFY(unread->isUnread());
        QVERIFY(unread->isUnreadDotShown());
        QCOMPARE(unread->number(), QStringLiteral("42"));
        QVERIFY(unread->accessibleName().contains("#42"));
        QVERIFY(unread->accessibleDescription().contains("plain-feature"));
        QVERIFY(unread->toolTip().contains("plain-feature"));
        QVERIFY(unread->toolTip().contains(unread->stateText()));
        QCOMPARE(waiting->status(), ket::ConversationStatus::NeedsInput);
        QVERIFY(!waiting->isUnread());
        auto* header = list.findChild<ket::SidebarRepositoryHeader*>();
        QCOMPARE(header->waitingCount(), 2);
        unread->click();
        QVERIFY(!unread->isUnread());
        QCOMPARE(header->waitingCount(), 1);
    }

    // The text edges and one-line rows hold at every zoom step and at the narrowest sidebar.
    void theLayoutHoldsAtEveryZoomAndWidth() {
        SidebarRepository plain{"/plain", "plain", false, {}};
        for (int index = 0; index < 7; ++index)
            plain.conversations.append(conversation(
                QStringLiteral("p%1").arg(index), QStringLiteral("sp%1").arg(index),
                QStringLiteral("A conversation with a title far too long for any sidebar %1")
                    .arg(index)));
        const auto previous = ket::Profile::current();
        ket::Profile::setCurrent(ket::Profile::ketPlus());
        for (const int percent : {100, 125, 150}) {
            ket::Appearance::instance().setZoom(InterfaceZoomMetrics::factor(percent));
            for (const int width : {ActivityBar::kMinWidth, ActivityBar::kWidth}) {
                const auto where = [&](const char* place, const QStringList& problems) {
                    return QStringLiteral("%1 at %2%, %3 px: %4")
                        .arg(QLatin1String(place))
                        .arg(percent)
                        .arg(width)
                        .arg(problems.join("; "));
                };
                SidebarRepositoryList list;
                list.applyZoom(percent);
                list.setRepositories({repository("/repo", 1), plain});
                list.resize(InterfaceZoomMetrics::geometry(width, percent), list.height());
                list.show();
                QCoreApplication::processEvents();
                QVERIFY2(list.layoutProblems().isEmpty(),
                         qPrintable(where("list", list.layoutProblems())));
                // The real sidebar too, with its margins and scroll area, at the same zoom.
                ThemeManager theme(*qApp);
                ActivityBar bar;
                bar.applyPalette(theme.palette(), percent);
                bar.setSidebarWidth(width);
                bar.resize(bar.width(), 700);
                bar.setRepositories({repository("/repo", 1), plain});
                bar.show();
                QVERIFY(QTest::qWaitForWindowExposed(&bar));
                auto* inBar = bar.findChild<SidebarRepositoryList*>();
                QVERIFY(inBar);
                // The list inside is no wider than the sidebar at this zoom: the narrow case runs.
                QVERIFY2(inBar->width() <= InterfaceZoomMetrics::geometry(width, percent),
                         qPrintable(where("width", {QString::number(inBar->width())})));
                QVERIFY2(inBar->layoutProblems().isEmpty(),
                         qPrintable(where("sidebar", inBar->layoutProblems())));
            }
        }
        ket::Profile::setCurrent(previous);
    }

    // The real sidebar, themed, light and dark: the tree holds its geometry there too.
    // KETPLUS_SIDEBAR_TREE_CAPTURE names a folder for the PR's images beside the mock.
    void theThemedSidebarHoldsInBothThemes() {
        ThemeManager theme(*qApp);
        SidebarRepository shop = repository("/repo", 1);
        shop.name = "ketplus";
        shop.teams.first().name = "Mobile Refactor";
        int memberNumber = 9420;
        for (QVariant& value : shop.teams.first().members) {
            QVariantMap member = value.toMap();
            member.insert("number", QString::number(memberNumber++));
            value = member;
        }
        const auto loose = [](const QString& id, const QString& title, const QString& number,
                              const QVariantMap& state) {
            QVariantMap row = conversation(id, "s-" + id, title);
            row.insert("number", number);
            row.insert(state);
            return row;
        };
        shop.conversations = {
            loose("w", "Sidebar tree from the kit", "9410", {{"running", true}}),
            loose("q", "Which zoom steps to test?", "9402", {{"needsInput", true}}),
            loose("u", "Pin ket-qt to the tree parts", "9398", {}),
            loose("d", "Lite edition build", "9387", {{"seen", true}}),
            loose("e", "Relay capability check", "9371", {{"status", "Failed"}, {"seen", true}})};
        SidebarRepository folded{"/sample-project",
                                 "sample-project",
                                 false,
                                 {loose("k", "Gateway logs", "8120", {{"needsInput", true}})}};
        QSettings().setValue("sidebar/collapsedRepositories", QStringList{"/sample-project"});
        const QString capture = qEnvironmentVariable("KETPLUS_SIDEBAR_TREE_CAPTURE");
        for (const bool dark : {false, true}) {
            theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
            ActivityBar bar;
            bar.applyPalette(theme.palette());
            bar.resize(ActivityBar::kWidth, 700);
            bar.setRepositories({shop, folded});
            bar.show();
            QVERIFY(QTest::qWaitForWindowExposed(&bar));
            auto* list = bar.findChild<SidebarRepositoryList*>();
            QVERIFY(list);
            QVERIFY2(list->layoutProblems().isEmpty(),
                     qPrintable(list->layoutProblems().join("; ")));
            if (!capture.isEmpty())
                QVERIFY(bar.grab().save(capture + (dark ? "/app-dark.png" : "/app-light.png")));
        }
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
        QCOMPARE(leave(team("7", 2), "s-rev").second,
                 QStringLiteral("A team keeps at least one reviewer: add another before removing "
                                "this one."));
        // T-23: one of two reviewers leaves.
        QJsonObject reviewed = team("7", 2);
        QJsonArray members = reviewed.value("members").toArray();
        members.append(member("s-rev2", "reviewer", "Review 2"));
        reviewed.insert("members", members);
        QVERIFY(leave(reviewed, "s-rev").first);
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
