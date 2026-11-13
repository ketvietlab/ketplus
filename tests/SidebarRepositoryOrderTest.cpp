// T-92: the desktop sidebar's repository order. A repository dates from the newest turn start
// of anything it shows, its teams' members included; the user pins repositories to the top; a
// conversation stays in the group of the repository it was created in, whatever worktree it
// runs in later. Offscreen, with settings in a temporary INI file and synthetic data only.
#include "KetAppearance.h"
#include "KetSidebarTree.h"
#include "KetStyle.h"
#include "agent/AgentSessionManager.h"
#include "agent/CodexAppServer.h"
#include "app/MainWindow.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"
#include "workspace/SidebarRepositoryList.h"

#include <QAbstractButton>
#include <QAction>
#include <QDateTime>
#include <QDir>
#include <QJsonObject>
#include <QMenu>
#include <QProcess>
#include <QSettings>
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

    // Criterion 2: a repository holding only team members dates from their newest turn; the
    // root cause was that recency read `conversations`, where team members never are.
    void aTeamOnlyRepositoryWithARecentTurnSortsAboveIdleOnes() {
        QVector<SidebarRepository> repositories{
            {.path = "/work/alpha", .name = "alpha"},
            {.path = "/work/beta", .name = "beta"},
            {.path = "/work/zed",
             .name = "zed",
             .teams = {team({member("lead", "leader", 10), member("dev", "developer", 500)})}}};
        QCOMPARE(ketplus::sidebarRepositoryRecency(repositories.at(2)), 500);
        ketplus::sortSidebarRepositories(repositories);
        QCOMPARE(pathsOf(repositories), (QStringList{"/work/zed", "/work/alpha", "/work/beta"}));
        // The members keep the order the team gave them.
        const QVariantList members = repositories.first().teams.first().members;
        QCOMPARE(members.at(0).toMap().value("id").toString(), QStringLiteral("lead"));
        QCOMPARE(members.at(1).toMap().value("id").toString(), QStringLiteral("dev"));
    }

    // Criterion 2: a mixed repository dates from the newer of its own and its team's turns,
    // and its own conversations keep their newest-first order.
    void aMixedRepositoryDatesFromItsNewestTurnOfEitherKind() {
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
        // Its own conversations only: older than "own", it sorts below it.
        repositories[0].teams.clear();
        ketplus::sortSidebarRepositories(repositories);
        QCOMPARE(pathsOf(repositories), (QStringList{"/work/own", "/work/mixed"}));
    }

    // Criterion 2: idle repositories follow by name, case-insensitively, then by path.
    void idleRepositoriesFollowByName() {
        QVector<SidebarRepository> repositories{{.path = "/b/zeta", .name = "zeta"},
                                                {.path = "/b/Alpha", .name = "Alpha"},
                                                {.path = "/a/alpha", .name = "alpha"},
                                                {.path = "/b/beta", .name = "beta"}};
        ketplus::sortSidebarRepositories(repositories);
        QCOMPARE(pathsOf(repositories),
                 (QStringList{"/a/alpha", "/b/Alpha", "/b/beta", "/b/zeta"}));
    }

    // Criteria 3 and 5: pinned repositories lead in the order they were pinned, then recency
    // and name; unpinning returns one to its place; pins survive a reload.
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

    // Evidence for the pull request, only when KETPLUS_SIDEBAR_CAPTURE names a folder: the
    // sidebar of the report (idle repositories, then one whose conversations are all team
    // members), with one repository pinned when KETPLUS_SIDEBAR_CAPTURE_PIN is set.
    void captureForThePullRequest() {
        const QString target = qEnvironmentVariable("KETPLUS_SIDEBAR_CAPTURE");
        if (target.isEmpty())
            QSKIP("Set KETPLUS_SIDEBAR_CAPTURE to a folder to write the captures.");
        QVector<SidebarRepository> repositories;
        for (const QString& name :
             {"kajima-php", "ket-ai-foundation", "ketatlas", "ketplus", "ketplus-app"})
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
