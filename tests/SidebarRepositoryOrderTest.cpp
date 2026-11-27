// Repository order stays independent of conversation/team activity; users can pin repositories
// to the top. A conversation stays in the group of the repository it was created in, whatever
// worktree it runs in later. Offscreen, with settings in a temporary INI file and synthetic data
// only.
#include "KetAppearance.h"
#include "KetSidebarTree.h"
#include "KetStyle.h"
#include "agent/AgentSessionManager.h"
#include "agent/CodexAppServer.h"
#include "app/MainWindow.h"
#include "teams/TeamDirectory.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"
#include "workspace/SidebarRepositoryList.h"

#include <QAbstractButton>
#include <QAction>
#include <QDateTime>
#include <QDir>
#include <QEnterEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <memory>

using ketplus::SidebarRepository;
using ketplus::SidebarTeam;

namespace {

QVariantMap conversation(const QString& id, const qint64 startedAt) {
    return {{QStringLiteral("id"), id},
            {QStringLiteral("title"), id},
            {QStringLiteral("started"), true},
            {QStringLiteral("seen"), true},
            {QStringLiteral("path"), QStringLiteral("/work/") + id},
            {QStringLiteral("lastTurnStartedAt"), startedAt}};
}

QVariantMap member(const QString& id, const QString& role, const qint64 startedAt) {
    QVariantMap row = conversation(id, startedAt);
    row.insert(QStringLiteral("sessionId"), id);
    row.insert(QStringLiteral("teamId"), QStringLiteral("1"));
    row.insert(QStringLiteral("teamRole"), role);
    return row;
}

SidebarTeam team(const QVariantList& members) {
    return {.id = QStringLiteral("1"), .name = QStringLiteral("Go"), .members = members};
}

QStringList pathsOf(const QVector<SidebarRepository>& repositories) {
    QStringList paths;
    for (const SidebarRepository& repository : repositories)
        paths << repository.path;
    return paths;
}

void git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git",
                  QStringList{"-c", "user.email=t@example.com", "-c", "user.name=T"} + arguments);
    QVERIFY(process.waitForFinished(15000));
    QCOMPARE(process.exitCode(), 0);
}

} // namespace

class SidebarRepositoryOrderTest final : public QObject {
    Q_OBJECT

  private:
    std::unique_ptr<QTemporaryDir> settings_;
    std::unique_ptr<QTemporaryDir> folders_;

    // A real folder, so a pin of it survives a reload.
    QString folder(const QString& name) {
        const QString path = QDir(folders_->path()).filePath(name);
        QDir().mkpath(path);
        return QFileInfo(path).canonicalFilePath();
    }
    QStringList shownOrder(const ketplus::SidebarRepositoryList& list) {
        QStringList paths;
        for (auto* group : list.findChildren<QWidget*>(QStringLiteral("productRepositoryGroup")))
            paths << group->property("repositoryPath").toString();
        return paths;
    }
    ket::SidebarRepositoryHeader* headerOf(ketplus::SidebarRepositoryList& list,
                                           const QString& path) {
        for (auto* group : list.findChildren<QWidget*>(QStringLiteral("productRepositoryGroup")))
            if (group->property("repositoryPath").toString() == path)
                return group->findChild<ket::SidebarRepositoryHeader*>(
                    QStringLiteral("productRepositoryGroupButton"));
        return nullptr;
    }

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
    }

    void init() {
        // Never the user's preferences (D-17): an INI file in a folder of this test.
        settings_ = std::make_unique<QTemporaryDir>();
        folders_ = std::make_unique<QTemporaryDir>();
        QVERIFY(settings_->isValid() && folders_->isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_->path());
        QSettings().clear();
        ket::Appearance::instance().setZoom(1.0);
    }

    void teamActivityDoesNotPromoteItsRepository() {
        QVector<SidebarRepository> repositories{
            {.path = "/work/alpha", .name = "alpha"},
            {.path = "/work/beta", .name = "beta"},
            {.path = "/work/zed",
             .name = "zed",
             .teams = {team({member("lead", "leader", 10), member("dev", "developer", 500)})}}};
        ketplus::sortSidebarRepositories(repositories);
        QCOMPARE(pathsOf(repositories), (QStringList{"/work/alpha", "/work/beta", "/work/zed"}));
        // The members keep the order the team gave them.
        const QVariantList members = repositories.last().teams.first().members;
        QCOMPARE(members.at(0).toMap().value("id").toString(), QStringLiteral("lead"));
        QCOMPARE(members.at(1).toMap().value("id").toString(), QStringLiteral("dev"));
    }

    void conversationsKeepTheirNewestFirstOrderWithinTheRepository() {
        QVector<SidebarRepository> repositories{
            {.path = "/work/own", .name = "own", .conversations = {conversation("own-new", 300)}},
            {.path = "/work/mixed",
             .name = "mixed",
             .conversations = {conversation("older", 100), conversation("newer", 200)},
             .teams = {team({member("dev", "developer", 400)})}}};
        ketplus::sortSidebarRepositories(repositories);
        QCOMPARE(pathsOf(repositories), (QStringList{"/work/mixed", "/work/own"}));
        QCOMPARE(repositories.first().conversations.first().toMap().value("id").toString(),
                 QStringLiteral("newer"));
        // Removing a team or starting a newer turn elsewhere cannot move repository groups.
        repositories[0].teams.clear();
        repositories[1].conversations = {conversation("own-new", 900)};
        ketplus::sortSidebarRepositories(repositories);
        QCOMPARE(pathsOf(repositories), (QStringList{"/work/mixed", "/work/own"}));
    }

    void repositoriesFollowByName() {
        QVector<SidebarRepository> repositories{{.path = "/b/zeta", .name = "zeta"},
                                                {.path = "/b/Alpha", .name = "Alpha"},
                                                {.path = "/a/alpha", .name = "alpha"},
                                                {.path = "/b/beta", .name = "beta"}};
        ketplus::sortSidebarRepositories(repositories);
        QCOMPARE(pathsOf(repositories),
                 (QStringList{"/a/alpha", "/b/Alpha", "/b/beta", "/b/zeta"}));
    }

    void activityUpdatesKeepRenderedRepositoryOrder_data() {
        QTest::addColumn<bool>("teamMember");
        QTest::newRow("conversation") << false;
        QTest::newRow("team-member") << true;
    }

    // Local and daemon projections both reach setRepositories. Render the real list before
    // and after status, selection and structural updates; keep explicit pin/unpin behavior.
    void activityUpdatesKeepRenderedRepositoryOrder() {
        QFETCH(bool, teamMember);
        const QString alpha = folder("alpha");
        const QString beta = folder("beta");
        const QString zed = folder("zed");
        QVector<SidebarRepository> repositories{{.path = alpha,
                                                 .name = "alpha",
                                                 .active = true,
                                                 .conversations = {conversation("a", 200)},
                                                 .pinnable = true},
                                                {.path = beta,
                                                 .name = "beta",
                                                 .conversations = {conversation("b", 100)},
                                                 .pinnable = true},
                                                {.path = zed, .name = "zed", .pinnable = true}};
        if (teamMember)
            repositories[2].teams = {team({member("z", "leader", 0)})};
        else
            repositories[2].conversations = {conversation("z", 0)};

        ketplus::SidebarRepositoryList list;
        list.resize(260, 600);
        list.setRepositories(repositories);
        list.show();
        QVERIFY(QTest::qWaitForWindowExposed(&list));
        const QStringList expected{alpha, beta, zed};
        QCOMPARE(shownOrder(list), expected);
        const QPointer<ket::SidebarRepositoryHeader> alphaHeader = headerOf(list, alpha);
        QVariantList& rows =
            teamMember ? repositories[2].teams[0].members : repositories[2].conversations;
        QVariantMap updated = rows[0].toMap();
        updated.insert("lastTurnStartedAt", 1000);
        updated.insert("running", true);
        updated.insert("selected", true);
        rows[0] = updated;
        repositories[0].active = false;
        repositories[2].active = true;
        list.setRepositories(repositories);
        QCOMPARE(shownOrder(list), expected);
        QCOMPARE(headerOf(list, alpha), alphaHeader.data());

        QSignalSpy requested(&list, &ketplus::SidebarRepositoryList::conversationRequested);
        QAbstractButton* selected = nullptr;
        for (auto* row : list.findChildren<QAbstractButton*>("productConversationButton"))
            if (row->property("conversationId").toString() == QStringLiteral("z"))
                selected = row;
        QVERIFY(selected);
        QVERIFY(selected->isChecked());
        selected->click();
        QCOMPARE(requested.size(), 1);
        QCOMPARE(requested.first(),
                 (QList<QVariant>{QStringLiteral("/work/z"), QStringLiteral("z")}));
        QCOMPARE(shownOrder(list), expected);

        updated.insert("title", "Updated title");
        rows[0] = updated;
        if (teamMember)
            repositories[2].teams[0].name = "Updated team";
        list.setRepositories(repositories);
        QCOMPARE(shownOrder(list), expected);
        list.setRepositoryPinned(zed, true);
        list.setRepositoryPinned(alpha, true);
        QCOMPARE(shownOrder(list), (QStringList{zed, alpha, beta}));
        list.setRepositories(repositories);
        QCOMPARE(shownOrder(list), (QStringList{zed, alpha, beta}));
        list.setRepositoryPinned(zed, false);
        QCOMPARE(shownOrder(list), expected);
    }

    // Pinned repositories lead in pin order, then name; unpinning returns one to its place.
    // Pins survive a reload.
    void pinnedRepositoriesLeadInTheOrderTheyWerePinned() {
        const QString busy = folder("busy");
        const QString quiet = folder("quiet");
        const QString later = folder("later");
        const QVector<SidebarRepository> repositories{
            {.path = busy,
             .name = "busy",
             .conversations = {conversation("busy-1", 900)},
             .pinnable = true},
            {.path = quiet, .name = "quiet", .pinnable = true},
            {.path = later, .name = "later", .pinnable = true}};
        {
            ketplus::SidebarRepositoryList list;
            list.resize(260, 600);
            list.setRepositories(repositories);
            QCOMPARE(shownOrder(list), (QStringList{busy, later, quiet}));
            QVERIFY(list.findChildren<QWidget*>("productRepositoryPinMark").isEmpty());

            list.setRepositoryPinned(quiet, true);
            list.setRepositoryPinned(later, true);
            QCOMPARE(shownOrder(list), (QStringList{quiet, later, busy}));
            QCOMPARE(list.findChildren<QWidget*>("productRepositoryPinMark").size(), 2);
            QVERIFY(headerOf(list, quiet)->findChild<QWidget*>("productRepositoryPinMark"));
            QVERIFY(!headerOf(list, busy)->findChild<QWidget*>("productRepositoryPinMark"));
            QCOMPARE(QSettings().value("sidebar/pinnedRepositories").toStringList(),
                     (QStringList{quiet, later}));
        }
        {
            // A new list, as after a restart, reads the pins back.
            ketplus::SidebarRepositoryList list;
            list.resize(260, 600);
            list.setRepositories(repositories);
            QCOMPARE(shownOrder(list), (QStringList{quiet, later, busy}));
            list.setRepositoryPinned(quiet, false);
            QCOMPARE(shownOrder(list), (QStringList{later, busy, quiet}));
            QCOMPARE(QSettings().value("sidebar/pinnedRepositories").toStringList(),
                     QStringList{later});
        }
    }

    // Criterion 3: the header's context menu pins and unpins; another machine's repository
    // offers nothing and never sorts as pinned.
    void theHeaderMenuPinsAndUnpins() {
        const QString shop = folder("shop");
        const QString other = folder("other");
        ketplus::SidebarRepositoryList list;
        list.resize(260, 600);
        list.setRepositories({{.path = other,
                               .name = "other",
                               .conversations = {conversation("o", 50)},
                               .pinnable = true},
                              {.path = shop, .name = "shop", .pinnable = true},
                              {.path = "/remote/repo", .name = "remote"}});
        QCOMPARE(headerOf(list, shop)->contextMenuPolicy(), Qt::CustomContextMenu);
        QCOMPARE(headerOf(list, "/remote/repo")->contextMenuPolicy(), Qt::DefaultContextMenu);
        QVERIFY(list.createRepositoryMenu("/remote/repo", &list) == nullptr);

        std::unique_ptr<QMenu> menu(list.createRepositoryMenu(shop, &list));
        QVERIFY(menu);
        auto* pin = menu->findChild<QAction*>("productRepositoryPinAction");
        QVERIFY(pin);
        QCOMPARE(pin->text(), QStringLiteral("Pin to top"));
        pin->trigger();
        QCOMPARE(shownOrder(list), (QStringList{shop, other, "/remote/repo"}));

        menu.reset(list.createRepositoryMenu(shop, &list));
        pin = menu->findChild<QAction*>("productRepositoryPinAction");
        QCOMPARE(pin->text(), QStringLiteral("Unpin"));
        pin->trigger();
        QCOMPARE(shownOrder(list), (QStringList{other, "/remote/repo", shop}));

        // A pin by path does not reach a repository that cannot be pinned.
        const QString remote = folder("remote");
        QSettings().setValue("sidebar/pinnedRepositories", QStringList{remote});
        ketplus::SidebarRepositoryList fresh;
        fresh.setRepositories({{.path = remote, .name = "remote"},
                               {.path = other, .name = "other", .pinnable = true}});
        QCOMPARE(fresh.pins().paths(), QStringList{remote});
        QCOMPARE(shownOrder(fresh), (QStringList{other, remote}));
    }

    // Criterion 3: a pinned repository whose folder is gone is dropped without a word.
    void aPinWhoseFolderIsGoneIsDropped() {
        const QString kept = folder("kept");
        const QString gone = folder("gone");
        QSettings().setValue("sidebar/pinnedRepositories", QStringList{gone, kept});
        QVERIFY(QDir(gone).removeRecursively());
        ketplus::SidebarRepositoryList list;
        QCOMPARE(list.pins().paths(), QStringList{kept});
        QCOMPARE(QSettings().value("sidebar/pinnedRepositories").toStringList(), QStringList{kept});
    }

    // Round 1 L1: the pin stays clear of the count and the hover action, at any name length;
    // a long name is shortened to make room, and the header keeps the full accessible name.
    void thePinMarkNeverMeetsTheCountOrTheAction() {
        const QString shortName = folder("shop");
        const QString longName = folder("long");
        const QString full = QStringLiteral("a-repository-name-far-too-long-for-the-sidebar-row");
        const QVector<SidebarRepository> repositories{
            {.path = shortName,
             .name = "shop",
             .conversations = {conversation("shop-1", 900)},
             .pinnable = true},
            {.path = longName,
             .name = full,
             .conversations = {conversation("long-1", 800), conversation("long-2", 700)},
             .pinnable = true}};
        ketplus::SidebarRepositoryList list;
        list.resize(260, 600);
        list.setRepositories(repositories);
        list.setRepositoryPinned(shortName, true);
        list.setRepositoryPinned(longName, true);
        list.show();
        QVERIFY(QTest::qWaitForWindowExposed(&list));
        const auto clear = [&](const QString& path, const char* state) {
            QCoreApplication::processEvents();
            ket::SidebarRepositoryHeader* header = headerOf(list, path);
            QVERIFY(header != nullptr);
            auto* mark = header->findChild<QWidget*>("productRepositoryPinMark");
            QVERIFY2(mark != nullptr, state);
            QVERIFY2(header->rect().contains(mark->geometry()), state);
            QVERIFY2(mark->geometry().right() <= header->nameRect().right(), state);
            for (QWidget* part : header->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
                if (part == mark || part->isHidden())
                    continue;
                QVERIFY2(!part->geometry().intersects(mark->geometry()),
                         qPrintable(QStringLiteral("%1: pin %2 meets %3 %4")
                                        .arg(QString::fromLatin1(state))
                                        .arg(QDebug::toString(mark->geometry()))
                                        .arg(QString::fromLatin1(part->metaObject()->className()))
                                        .arg(QDebug::toString(part->geometry()))));
            }
        };
        clear(shortName, "short name");
        clear(longName, "long name");
        ket::SidebarRepositoryHeader* header = headerOf(list, longName);
        QVERIFY(header->text() != full);
        QCOMPARE(header->accessibleName(), full);
        header->setExpanded(!header->isExpanded());
        QCOMPARE(header->accessibleName(), full);
        // Hovered, the header shows its action where the name area ends.
        QEnterEvent enter(QPointF(10, 10), QPointF(10, 10), QPointF(10, 10));
        QCoreApplication::sendEvent(header, &enter);
        clear(longName, "long name, hovered");
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(header, &leave);
        clear(longName, "long name, left");
    }

    // Evidence for the pull request, only when KETPLUS_SIDEBAR_CAPTURE names a folder: the
    // sidebar of the report (idle repositories, then one whose conversations are all team
    // members), with one repository pinned when KETPLUS_SIDEBAR_CAPTURE_PIN is set.
    void captureForThePullRequest() {
        const QString target = qEnvironmentVariable("KETPLUS_SIDEBAR_CAPTURE");
        if (target.isEmpty())
            QSKIP("Set KETPLUS_SIDEBAR_CAPTURE to a folder to write the captures.");
        QVector<SidebarRepository> repositories;
        for (const QString& name : {"api-server", "atlas", "design-kit", "docs-site", "ketplus"})
            repositories.append({.path = folder(name), .name = name, .pinnable = true});
        SidebarTeam golive{.id = "5", .name = "Go Live"};
        golive.members = {member("lead", "leader", 1700), member("dev-4", "developer", 1900),
                          member("rev", "reviewer", 1500)};
        golive.members[1] = [&] {
            QVariantMap row = golive.members[1].toMap();
            row.insert("running", true);
            return row;
        }();
        repositories.append({.path = folder("ketplus-desktop"),
                             .name = "ketplus-desktop",
                             .teams = {golive},
                             .pinnable = true});
        ketplus::ActivityBar bar;
        bar.resize(300, 560);
        bar.show();
        auto* list = bar.repositoryList();
        if (qEnvironmentVariableIsSet("KETPLUS_SIDEBAR_CAPTURE_PIN"))
            list->setRepositoryPinned(
                repositories
                    .at(qEnvironmentVariableIntValue("KETPLUS_SIDEBAR_CAPTURE_PIN") == 2 ? 1 : 3)
                    .path,
                true);
        bar.setRepositories(repositories);
        QTest::qWait(200);
        QDir().mkpath(target);
        QVERIFY(list->grab().save(QDir(target).filePath(
            qEnvironmentVariable("KETPLUS_SIDEBAR_CAPTURE_NAME", "sidebar") + ".png")));
    }

    void explicitPlacementSurvivesWindowChangesAndFilter_data() {
        QTest::addColumn<bool>("noRepository");
        QTest::newRow("known-no-repository") << true;
        QTest::newRow("real-repository-named-repository") << false;
    }
    void explicitPlacementSurvivesWindowChangesAndFilter() {
        QFETCH(bool, noRepository);
        const auto origin = folder("repository");
        const auto execution = folder("execution");
        for (const auto& root :
             noRepository ? QStringList{execution} : QStringList{origin, execution}) {
            git(root, {"init", "-q", "-b", "main"});
            git(root, {"commit", "-q", "--allow-empty", "-m", "fixture"});
        }
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(execution);
        window.resize(1200, 800);
        window.show();
        auto* agent = window.findChild<QAbstractButton*>("agentWorkspaceActivityButton");
        QVERIFY(agent);
        agent->click();
        auto* server = window.findChild<ketplus::CodexAppServer*>();
        auto* list = window.findChild<ketplus::SidebarRepositoryList*>();
        QVERIFY(server);
        QVERIFY(list);
        QJsonObject session{{"session_id", "placement-row"},
                            {"session_generation", 1},
                            {"conversation_path", origin},
                            {"worktree_path", execution},
                            {"sidebar_repository_path", noRepository ? QString{} : origin},
                            {"title", "Stable placement fixture"},
                            {"state", "idle"}};
        emit server->daemonSessionDiscovered(session);
        const auto row = [&]() -> QAbstractButton* {
            for (auto* item : window.findChildren<QAbstractButton*>("productConversationButton"))
                if (item->property("conversationId") == QStringLiteral("placement-row"))
                    return item;
            return nullptr;
        };
        QTRY_VERIFY_WITH_TIMEOUT(row() != nullptr, 10000);
        const auto placement = noRepository ? QString{} : origin;
        QCOMPARE(row()->property("repositoryPath").toString(), placement);
        list->setFilterText("Stable placement");
        QVERIFY(row());
        row()->click();
        QTRY_VERIFY_WITH_TIMEOUT(row() != nullptr, 10000);
        QCOMPARE(row()->property("repositoryPath").toString(), placement);
        QCOMPARE(list->filterText(), QStringLiteral("Stable placement"));
        window.openFolder(origin);
        emit server->daemonSessionDiscovered(session);
        QTRY_VERIFY_WITH_TIMEOUT(row() != nullptr, 10000);
        QCOMPARE(row()->property("repositoryPath").toString(), placement);
        QCOMPARE(list->filterText(), QStringLiteral("Stable placement"));
        // A later execution update must not rewrite a known placement of this generation.
        session.insert("worktree_path", origin);
        session.insert("sidebar_repository_path", execution);
        emit server->daemonSessionDiscovered(session);
        QTRY_VERIFY_WITH_TIMEOUT(row() != nullptr, 10000);
        QCOMPARE(row()->property("repositoryPath").toString(), placement);
        list->setFilterText({});
        QTRY_VERIFY(row() != nullptr);
        QCOMPARE(row()->property("repositoryPath").toString(), placement);
        if (noRepository)
            QVERIFY(!shownOrder(*list).contains(origin));
        const auto captures = qEnvironmentVariable("KETPLUS_ORIGIN_CAPTURE_DIR");
        if (!captures.isEmpty()) {
            QVERIFY(QDir().mkpath(captures));
            QVERIFY(window.grab().save(QDir(captures).filePath(
                noRepository ? QStringLiteral("local-no-repository.png")
                             : QStringLiteral("local-repository-identity.png"))));
        }
    }

    void teamMembersStayTogetherWhenExecutionAndTheTeamRepositoryChange() {
        const auto origin = folder("origin");
        const auto execution = folder("execution");
        for (const auto& root : {origin, execution}) {
            git(root, {"init", "-q", "-b", "main"});
            git(root, {"commit", "-q", "--allow-empty", "-m", "fixture"});
        }
        QJsonObject team{
            {"id", "7"},
            {"revision", 1},
            {"name", "Origin team"},
            {"repository", origin},
            {"sidebar_repository", origin},
            {"members",
             QJsonArray{
                 QJsonObject{{"session_id", "origin-lead"}, {"role", "leader"}, {"name", "Lead"}},
                 QJsonObject{
                     {"session_id", "origin-dev"}, {"role", "developer"}, {"name", "Developer"}}}}};
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(execution);
        window.resize(1200, 800);
        window.show();
        auto* agent = window.findChild<QAbstractButton*>("agentWorkspaceActivityButton");
        QVERIFY(agent);
        agent->click();
        auto* server = window.findChild<ketplus::CodexAppServer*>();
        auto* directory = window.findChild<ketplus::TeamDirectory*>();
        auto* list = window.findChild<ketplus::SidebarRepositoryList*>();
        QVERIFY(server);
        QVERIFY(directory);
        QVERIFY(list);
        directory->setTransport(
            [&team](const QString&, const QJsonObject&, ketplus::TeamDirectory::Reply reply) {
                reply({{"ok", true},
                       {"team", team},
                       {"teams", QJsonArray{team}},
                       {"tasks", QJsonArray{}},
                       {"updates", QJsonArray{}},
                       {"decisions", QJsonArray{}}},
                      {});
            });
        directory->setAvailable(true);
        for (const auto* id : {"origin-lead", "origin-dev"})
            emit server->daemonSessionDiscovered(
                QJsonObject{{"session_id", id},
                            {"session_generation", 1},
                            {"conversation_path", origin},
                            {"worktree_path", execution},
                            {"sidebar_repository_path", origin},
                            {"title", QStringLiteral("Stable ") + QLatin1String(id)},
                            {"state", "idle"}});
        const auto folderOfTeam = [&]() -> QWidget* {
            for (auto* item : window.findChildren<QWidget*>("productTeamFolder"))
                if (item->property("teamId") == QStringLiteral("7"))
                    return item;
            return nullptr;
        };
        QTRY_VERIFY_WITH_TIMEOUT(folderOfTeam() != nullptr, 10000);
        QCOMPARE(folderOfTeam()->property("repositoryPath").toString(), origin);
        QCOMPARE(folderOfTeam()->findChildren<QAbstractButton*>("productConversationButton").size(),
                 2);
        team.insert("revision", 2);
        team.insert("repository", execution);
        directory->applyEvent("team.changed", {{"team", team}});
        QTRY_VERIFY(folderOfTeam() != nullptr);
        QCOMPARE(folderOfTeam()->property("repositoryPath").toString(), origin);
        list->setFilterText("origin-dev");
        QTRY_VERIFY(folderOfTeam() != nullptr);
        auto* open = folderOfTeam()->findChild<QAbstractButton*>("productTeamFolderName");
        QVERIFY(open);
        open->click();
        QTRY_VERIFY(folderOfTeam() != nullptr);
        QCOMPARE(folderOfTeam()->property("repositoryPath").toString(), origin);
        QCOMPARE(list->filterText(), QStringLiteral("origin-dev"));
        QVERIFY(directory->teamOf("origin-dev"));
        QCOMPARE(directory->teamOf("origin-dev")->value("id").toString(), QStringLiteral("7"));
        list->setFilterText({});
        QTRY_COMPARE(
            folderOfTeam()->findChildren<QAbstractButton*>("productConversationButton").size(), 2);
        const auto captures = qEnvironmentVariable("KETPLUS_ORIGIN_CAPTURE_DIR");
        if (!captures.isEmpty()) {
            QVERIFY(QDir().mkpath(captures));
            QVERIFY(window.grab().save(QDir(captures).filePath("local-team-stable.png")));
        }
    }

    // Criteria 4 and 5: a conversation that switches its worktree to another repository stays
    // in the group of the repository it was created in; the other repository is not promoted.
    void aConversationKeepsTheGroupItWasCreatedIn() {
        const QString alpha = folder("alpha");
        const QString beta = folder("beta");
        for (const QString& repository : {alpha, beta}) {
            git(repository, {"init", "-q", "-b", "main"});
            git(repository, {"commit", "-q", "--allow-empty", "-m", "start"});
        }
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(alpha);
        window.resize(1200, 800);
        window.show();
        auto* agent =
            window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
        QVERIFY(agent);
        agent->click();
        auto* appServer = window.findChild<ketplus::CodexAppServer*>();
        QVERIFY(appServer);
        const auto started = [](const qint64 seconds) {
            return QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC)
                .toString(Qt::ISODateWithMs);
        };
        // Born in alpha, now running in beta (the worktree picker or a team task moved it).
        emit appServer->daemonSessionDiscovered(
            QJsonObject{{QStringLiteral("session_id"), QStringLiteral("switched")},
                        {QStringLiteral("session_generation"), 1},
                        {QStringLiteral("conversation_path"), alpha},
                        {QStringLiteral("worktree_path"), beta},
                        {QStringLiteral("title"), QStringLiteral("Switched")},
                        {QStringLiteral("last_turn_started_at"), started(2000000000)},
                        {QStringLiteral("state"), QStringLiteral("idle")}});
        const auto groupOf = [&window](const QString& id) {
            for (auto* row :
                 window.findChildren<QAbstractButton*>(QStringLiteral("productConversationButton")))
                if (row->property("conversationId").toString() == id)
                    return row->property("repositoryPath").toString();
            return QString();
        };
        QTRY_COMPARE_WITH_TIMEOUT(groupOf(QStringLiteral("switched")), alpha, 10000);
        QTest::qWait(300);
        QCOMPARE(groupOf(QStringLiteral("switched")), alpha);
        auto* list = window.findChild<ketplus::SidebarRepositoryList*>();
        QVERIFY(list);
        const QStringList order = shownOrder(*list);
        QCOMPARE(order.first(), alpha);
        QVERIFY(!order.contains(beta) || order.indexOf(beta) > order.indexOf(alpha));
    }
};

QTEST_MAIN(SidebarRepositoryOrderTest)
#include "SidebarRepositoryOrderTest.moc"
