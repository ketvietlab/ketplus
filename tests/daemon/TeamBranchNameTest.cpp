// T-93 (ADR 0123 §9): a new task's branch is `<type>/<slug>-t<n>` and its worktree
// `<repository>-<type>-<slug>-t<n>`, named by the leader or derived from the title; a task
// from before keeps `team/<id>/t-<n>`. The coordinator cases use a fake runtime; the git cases
// temporary repositories with a bare remote; the leader's tools the session runtime with the
// scripted fake providers. No network, no real provider and no real team.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "runtime/TeamGit.h"
#include "runtime/TeamWorktreeRemoval.h"
#include "storage/LocalJournal.h"
#include "teams/TeamBranchNames.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace ketplus::daemon;
using namespace team_test;
namespace names = teams::branch_names;

namespace {

QString git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git",
                  QStringList{"-c", "user.email=t@example.com", "-c", "user.name=T"} + arguments);
    if (!process.waitForFinished(15000) || process.exitCode() != 0) {
        qWarning("%s", process.readAllStandardError().constData());
        return QStringLiteral("<failed>");
    }
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

// A repository on `integration` with one commit and a bare origin that has it.
QString makeRepository(const QString& root, const QString& name) {
    const QString path = QDir(root).filePath(name);
    QDir().mkpath(path);
    git(path, {"init", "-q", "-b", "integration"});
    git(path, {"commit", "-q", "--allow-empty", "-m", "start"});
    const QString remote = QDir(root).filePath(name + ".git");
    git(root, {"init", "-q", "--bare", remote});
    git(path, {"remote", "add", "origin", remote});
    git(path, {"push", "-q", "-u", "origin", "integration"});
    return QFileInfo(path).canonicalFilePath();
}

Team otherTeam() {
    Team team = standardTeam();
    team.name = QStringLiteral("Second");
    team.members = {{.sessionId = "lead2", .role = role::leader, .name = "Lead 2"},
                    {.sessionId = "dev3", .role = role::developer, .name = "Dev 3"},
                    {.sessionId = "rev2", .role = role::reviewer, .name = "Reviewer 2"}};
    return team;
}

} // namespace

class TeamBranchNameTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    const Team& team(const qint64 id = 0) { return *teams_->team(id ? id : team_); }
    const Task& get(const QString& id) { return *team().task(id); }
    QString create(const TaskDraft& draft, TeamResult* result = nullptr, const qint64 teamId = 0,
                   const QString& actor = QStringLiteral("lead")) {
        QString id;
        const TeamResult made = teams_->createTask(teamId ? teamId : team_, draft, actor, &id);
        if (result)
            *result = made;
        return id;
    }
    QString planned(const QString& id) { return names::plannedBranch(team_, get(id)); }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        teams_.reset();
        journal_.reset();
        QFile::remove(directory_.filePath("journal.sqlite3"));
        host_ = {};
        for (const QString& id : {"lead", "dev1", "dev2", "lead2", "dev3"})
            host_.add(id);
        host_.add("rev", "claude");
        host_.add("rev2", "claude");
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(60 * 60 * 1000);
        teams_->setPullRequestPeriod(60 * 60 * 1000, 0);
        teams_->load();
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
    }

    // Criterion 1: derived from Vietnamese and English titles.
    void derivesTheNameFromTheTitle_data() {
        QTest::addColumn<QString>("title");
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("slug");
        QTest::newRow("plain English") << "Team task branch names" << "feat"
                                       << "team-task-branch-names";
        QTest::newRow("type prefix, aside")
            << "Fix: sidebar repo ordering (ketplus#92)" << "fix" << "sidebar-repo-ordering";
        QTest::newRow("defect word") << "Desktop sidebar crash when the list is empty" << "fix"
                                     << "desktop-sidebar-crash-list-empty";
        QTest::newRow("Vietnamese fix") << "Sửa lỗi đăng nhập trên iOS" << "fix"
                                        << "dang-nhap-ios";
        QTest::newRow("Vietnamese feature")
            << "Thêm màn hình Đội cho điện thoại" << "feat" << "them-man-hinh-doi-dien-thoai";
        // "lời" is not "lỗi", and "đề" is not the stop word "để".
        QTest::newRow("Vietnamese lookalikes")
            << "Trả lời đề xuất của leader" << "feat" << "tra-loi-de-xuat-leader";
        QTest::newRow("Vietnamese test") << "Kiểm thử vòng đời nhóm" << "test" << "vong-doi-nhom";
        QTest::newRow("decomposed input")
            << QString::fromUtf8("Sửa lỗi đăng nhập").normalized(QString::NormalizationForm_D)
            << "fix" << "dang-nhap";
        QTest::newRow("docs") << "Docs: ADR for task branch names" << "docs"
                              << "task-branch-names";
        QTest::newRow("task id first") << "T-47: refactor TeamGit into smaller files" << "refactor"
                                       << "teamgit-smaller-files";
        QTest::newRow("chore") << "Bump Qt to 6.10" << "chore" << "qt";
        QTest::newRow("cut at a word")
            << "Make the desktop sidebar order repositories by the newest conversation of "
               "every member"
            << "feat" << "make-desktop-sidebar-order-repositories";
        QTest::newRow("one long word")
            << "Supercalifragilisticexpialidociousandthensomemore" << "feat"
            << "supercalifragilisticexpialidociousandthe";
        QTest::newRow("nothing left") << "!!! 42" << "feat" << "task";
    }
    void derivesTheNameFromTheTitle() {
        QFETCH(QString, title);
        QFETCH(QString, type);
        QFETCH(QString, slug);
        QCOMPARE(names::typeOf(title), type);
        QCOMPARE(names::slugOf(title), slug);
        QVERIFY(names::slugOf(title).size() <= names::maximumSlug);
        QVERIFY(!names::validSlug(names::slugOf(title)));
    }

    // Criterion 1, through the rules: a new task gets the name, and the planned branch and
    // worktree read it.
    void aNewTaskIsNamedFromItsTitle() {
        const QString id = create({.title = "Sửa lỗi đăng nhập trên iOS", .lane = "Web"});
        QCOMPARE(id, QStringLiteral("T-1"));
        QCOMPARE(get(id).branchType, QStringLiteral("fix"));
        QCOMPARE(get(id).branchSlug, QStringLiteral("dang-nhap-ios"));
        QCOMPARE(planned(id), QStringLiteral("fix/dang-nhap-ios-t1"));
        QCOMPARE(names::plannedWorktreeName("shop", team_, get(id)),
                 QStringLiteral("shop-fix-dang-nhap-ios-t1"));
        QVERIFY(get(id).history.first().text.contains("fix/dang-nhap-ios-t1"));
        // Saved and read back.
        const Task again = teams::teamFromJson(teams::toJson(team())).tasks.first();
        QCOMPARE(again.branchType, QStringLiteral("fix"));
        QCOMPARE(again.branchSlug, QStringLiteral("dang-nhap-ios"));
    }

    // Criterion 2: the leader's type and slug, each optional.
    void anExplicitTypeAndSlugAreKept() {
        const QString both = create(
            {.title = "Upgrade the toolchain", .branchType = "chore", .branchSlug = "bump-qt"});
        QCOMPARE(planned(both), QStringLiteral("chore/bump-qt-t1"));
        const QString typeOnly = create({.title = "Upgrade the toolchain", .branchType = "chore"});
        QCOMPARE(planned(typeOnly), QStringLiteral("chore/upgrade-toolchain-t2"));
        const QString slugOnly = create({.title = "Fix the toolchain", .branchSlug = "toolchain"});
        QCOMPARE(planned(slugOnly), QStringLiteral("fix/toolchain-t3"));
    }

    // Criterion 3: invalid names are refused and take no task number.
    void anInvalidNameIsRefused_data() {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("slug");
        QTest::newRow("unknown type") << "feature" << "";
        QTest::newRow("upper case") << "" << "Bad-Slug";
        QTest::newRow("underscore") << "" << "bad_slug";
        QTest::newRow("double hyphen") << "" << "a--b";
        QTest::newRow("leading hyphen") << "" << "-ab";
        QTest::newRow("slash") << "" << "a/b";
        QTest::newRow("diacritics") << "" << "sửa";
        QTest::newRow("too long") << "" << QString(41, QLatin1Char('a'));
    }
    void anInvalidNameIsRefused() {
        QFETCH(QString, type);
        QFETCH(QString, slug);
        TeamResult result;
        create({.title = "Cart", .branchType = type, .branchSlug = slug}, &result);
        QVERIFY(!result.ok());
        QCOMPARE(result.error->code, QStringLiteral("invalid_payload"));
        QVERIFY(team().tasks.isEmpty());
        QCOMPARE(create({.title = "Cart"}), QStringLiteral("T-1"));
    }

    // Criterion 3: a name git or another task already has is refused.
    void aTakenNameIsRefused() {
        host_.takenBranches.insert("/repo|fix/login-t1");
        TeamResult result;
        create({.title = "Fix login"}, &result);
        QVERIFY(!result.ok());
        QCOMPARE(result.error->code, QStringLiteral("branch_taken"));
        QVERIFY(result.error->message.contains("fix/login-t1"));
        QVERIFY(result.error->message.contains("branch_slug"));
        const QString renamed = create({.title = "Fix login", .branchSlug = "login-again"});
        QCOMPARE(planned(renamed), QStringLiteral("fix/login-again-t1"));

        // A second team in the same repository counts its own T-1 the same way.
        const qint64 second = teams_->create(otherTeam(), "user").teamId;
        QVERIFY(second > 0);
        create({.title = "Fix login", .branchSlug = "login-again"}, &result, second, "lead2");
        QCOMPARE(result.error ? result.error->code : QString(), QStringLiteral("branch_taken"));
        QVERIFY(result.error->message.contains(QStringLiteral("T-1's of team %1").arg(team_)));
        QVERIFY(create({.title = "Fix login", .branchSlug = "login-mobile"}, &result, second,
                       "lead2") == QStringLiteral("T-1"));
        // Another repository's branches are its own.
        host_.takenBranches.insert("/elsewhere|feat/cart-t2");
        QCOMPARE(planned(create({.title = "Cart"})), QStringLiteral("feat/cart-t2"));
    }

    // Review L1: before the repository's branches are listed, the name is accepted and the
    // history says it was not checked against them yet.
    void aNameNotYetCheckedSaysSo() {
        host_.unlistedRepositories.insert("/repo");
        const QString id = create({.title = "Fix login"});
        QCOMPARE(planned(id), QStringLiteral("fix/login-t1"));
        QVERIFY(get(id).history.first().text.contains("not listed yet"));
        host_.unlistedRepositories.clear();
        const QString checked = create({.title = "Cart"});
        QVERIFY(!get(checked).history.first().text.contains("not listed yet"));
    }

    // Review N1: names git refuses as a directory/file conflict clash.
    void folderNamesClash() {
        QVERIFY(names::refsClash("feat", "feat/cart-t1"));
        QVERIFY(names::refsClash("fix/x-t1/sub", "fix/x-t1"));
        QVERIFY(names::refsClash("fix/x-t1", "fix/x-t1"));
        QVERIFY(!names::refsClash("feat/cart-t1", "feat/cart-t12"));
        QVERIFY(!names::refsClash("feat/cart", "feat/cart-t1"));
        QVERIFY(!names::refsClash("", "feat"));
        QVERIFY(names::clashesWithAny("feat/cart-t1", {"integration", "feat"}));
        QVERIFY(!names::clashesWithAny("feat/cart-t1", {"integration", "fix"}));
    }

    // Criterion 4: renamed by the leader before the branch exists, refused after.
    void aBranchIsRenamedOnlyBeforeItStarts() {
        const QString id = create({.title = "Cart", .lane = "Web"});
        QCOMPARE(planned(id), QStringLiteral("feat/cart-t1"));
        TeamResult result = teams_->updateTask(team_, id, {{"branch_slug", "basket"}}, "dev1");
        QCOMPARE(result.error ? result.error->code : QString(), QStringLiteral("role_forbidden"));
        result = teams_->updateTask(team_, id, {{"branch_slug", "Basket"}}, "lead");
        QCOMPARE(result.error ? result.error->code : QString(), QStringLiteral("invalid_payload"));
        QCOMPARE(planned(id), QStringLiteral("feat/cart-t1"));

        result = teams_->updateTask(team_, id, {{"branch_type", "fix"}, {"branch_slug", "basket"}},
                                    "lead");
        QVERIFY2(result.ok(), result.ok() ? "" : qPrintable(result.error->message));
        QCOMPARE(planned(id), QStringLiteral("fix/basket-t1"));
        QCOMPARE(get(id).history.last().kind, QStringLiteral("branch"));
        QCOMPARE(get(id).history.last().text,
                 QStringLiteral("Branch feat/cart-t1 → fix/basket-t1."));

        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QVERIFY(!get(id).branch.isEmpty());
        result = teams_->updateTask(team_, id, {{"branch_slug", "trolley"}}, "lead");
        QCOMPARE(result.error ? result.error->code : QString(), QStringLiteral("branch_started"));
        QCOMPARE(get(id).branchSlug, QStringLiteral("basket"));
    }

    // Criterion 5: a task from before keeps `team/<id>/t-<n>`; the leader may still name it
    // before it starts.
    void aTaskFromBeforeKeepsItsName() {
        Task old;
        old.id = QStringLiteral("T-3");
        QCOMPARE(names::plannedBranch(7, old), QStringLiteral("team/7/t-3"));
        QCOMPARE(names::plannedWorktreeName("shop", 7, old), QStringLiteral("shop-team-7-t-3"));
        const QJsonObject json = teams::toJson(old);
        QVERIFY(!json.contains("branch_slug"));
        QVERIFY(!json.contains("branch_type"));
    }

    // Criterion 5 and the git half of 1: the branch and worktree git makes for both kinds of
    // task, and the whole life after it: merged, detected as merged and cleaned up.
    void gitRunsTheLifecycleOfBothNames_data() {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("slug");
        QTest::addColumn<QString>("branch");
        QTest::addColumn<QString>("folder");
        QTest::newRow("new") << "fix" << "dang-nhap" << "fix/dang-nhap-t4"
                             << "shop-fix-dang-nhap-t4";
        QTest::newRow("from before") << "" << "" << "team/7/t-4" << "shop-team-7-t-4";
    }
    void gitRunsTheLifecycleOfBothNames() {
        QFETCH(QString, type);
        QFETCH(QString, slug);
        QFETCH(QString, branch);
        QFETCH(QString, folder);
        QTemporaryDir root;
        const QString shop = makeRepository(root.path(), "shop");
        Team team;
        team.id = 7;
        team.repository = shop;
        team.integrationBranch = QStringLiteral("integration");
        Task task;
        task.id = QStringLiteral("T-4");
        task.branchType = type;
        task.branchSlug = slug;
        const auto made = team_git::createWorktree(team, task);
        QVERIFY2(made.worktree, qPrintable(made.error));
        QCOMPARE(made.worktree->branch, branch);
        QCOMPARE(QFileInfo(made.worktree->worktree).fileName(), folder);
        task.branch = made.worktree->branch;
        task.worktree = made.worktree->worktree;
        task.baseCommit = made.worktree->baseCommit;
        QCOMPARE(team_git::branchExists(team, task), std::optional<bool>(true));
        QCOMPARE(team_git::branchTaken(shop, branch), std::optional<bool>(true));

        git(task.worktree, {"commit", "-q", "--allow-empty", "-m", "work"});
        QCOMPARE(team_git::isMerged(team, task), std::optional<bool>(false));
        git(task.worktree, {"push", "-q", "origin", branch});
        git(shop, {"merge", "-q", "--no-ff", "-m", "merge", branch});
        git(shop, {"push", "-q", "origin", "integration"});
        QCOMPARE(team_git::isMerged(team, task), std::optional<bool>(true));
        const auto facts = team_git::facts(team, {task}, teams::TaskQuestion::Merge);
        QVERIFY(facts.contains(task.id));
        QCOMPARE(facts.value(task.id).merged, std::optional<bool>(true));
        QCOMPARE(team_git::removeWorktree(shop, task.worktree), QString());
        QVERIFY(!QFileInfo::exists(task.worktree));
    }

    // Criterion 3, in git: local and origin's branches are taken, and a branch made after the
    // task was named stops its worktree with a sentence that says how to rename it.
    void gitSeesATakenBranch() {
        QTemporaryDir root;
        const QString shop = makeRepository(root.path(), "shop");
        QCOMPARE(team_git::branchTaken(shop, "fix/login-t1"), std::optional<bool>(false));
        git(shop, {"branch", "fix/login-t1"});
        QCOMPARE(team_git::branchTaken(shop, "fix/login-t1"), std::optional<bool>(true));
        // Only on origin, as another clone pushed it.
        git(shop, {"push", "-q", "origin", "integration:feat/remote-t2"});
        git(shop, {"fetch", "-q", "origin"});
        QCOMPARE(team_git::branchTaken(shop, "feat/remote-t2"), std::optional<bool>(true));
        QCOMPARE(team_git::branchTaken(root.filePath("missing"), "feat/remote-t2"), std::nullopt);
        QCOMPARE(team_git::branchNames(shop),
                 std::optional<QSet<QString>>(
                     QSet<QString>{"integration", "fix/login-t1", "feat/remote-t2"}));
        // Review N1: a branch that is a folder of the name, or the other way round.
        git(shop, {"branch", "chore"});
        QCOMPARE(team_git::branchTaken(shop, "chore/deps-t3"), std::optional<bool>(true));
        git(shop, {"branch", "docs/guide-t4/draft"});
        QCOMPARE(team_git::branchTaken(shop, "docs/guide-t4"), std::optional<bool>(true));
        QCOMPARE(team_git::branchTaken(shop, "docs/guide-t5"), std::optional<bool>(false));
        git(shop, {"branch", "-d", "chore"});
        git(shop, {"branch", "-d", "docs/guide-t4/draft"});

        Team team;
        team.id = 7;
        team.repository = shop;
        team.integrationBranch = QStringLiteral("integration");
        Task task;
        task.id = QStringLiteral("T-1");
        task.branchType = QStringLiteral("fix");
        task.branchSlug = QStringLiteral("login");
        const auto made = team_git::createWorktree(team, task);
        QVERIFY(!made.worktree);
        QVERIFY(made.error.contains("fix/login-t1 already exists"));
        QVERIFY(made.error.contains("branch_slug"));
        QVERIFY(!QFileInfo::exists(QDir(root.path()).filePath("shop-fix-login-t1")));
    }

    // The leader's tools: team_task_create names the branch, team_task_update renames it.
    void theLeaderNamesBranchesWithItsTools() {
        agent_test::Fixture fixture;
        QVERIFY(fixture.open());
        const QString shop = makeRepository(fixture.directory.path(), "shop");
        // Made before the runtime first lists the repository's branches (below).
        git(shop, {"branch", "fix/probe-t1"});
        const QJsonObject created = fixture.host->handleCommand(
            agent_test::request(
                "team.create",
                {{"repository", shop},
                 {"name", "QA"},
                 {"integration_branch", "integration"},
                 {"plan",
                  QJsonObject{{"leader", QJsonObject{{"provider", "claude"}}},
                              {"developers", QJsonObject{{"count", 1}, {"provider", "codex"}}},
                              {"reviewer", QJsonObject{{"provider", "claude"}}}}}}),
            "desktop-test");
        const QJsonArray members = created.value("team").toObject().value("members").toArray();
        QCOMPARE(members.size(), 3);
        const QString leader = members.at(0).toObject().value("session_id").toString();
        const QString developer = members.at(1).toObject().value("session_id").toString();

        const QJsonObject named = fixture.agents(
            leader, "agents.team_task_create",
            {{"title", "Login"}, {"branch_type", "fix"}, {"branch_slug", "sign-in"}});
        QVERIFY2(named.value("ok").toBool(), qPrintable(agent_test::text(named)));
        const QJsonObject task = named.value("task").toObject();
        QCOMPARE(task.value("branch_type").toString(), QStringLiteral("fix"));
        QCOMPARE(task.value("branch_slug").toString(), QStringLiteral("sign-in"));
        const QString id = task.value("id").toString();

        const QJsonObject derived =
            fixture.agents(leader, "agents.team_task_create", {{"title", "Thêm giỏ hàng"}});
        QCOMPARE(derived.value("task").toObject().value("branch_slug").toString(),
                 QStringLiteral("them-gio-hang"));

        const QJsonObject wrong = fixture.agents(leader, "agents.team_task_create",
                                                 {{"title", "Cart"}, {"branch_type", "feature"}});
        QCOMPARE(wrong.value("error_code").toString(), QStringLiteral("invalid_payload"));

        // The runtime reads the repository's branches off the event loop and, once that
        // listing is there, refuses a name git already has. Renaming T-1 is the probe: before
        // the listing it is simply renamed.
        QTRY_COMPARE_WITH_TIMEOUT(fixture
                                      .agents(leader, "agents.team_task_update",
                                              {{"task_id", id}, {"branch_slug", "probe"}})
                                      .value("error_code")
                                      .toString(),
                                  QStringLiteral("branch_taken"), 30000);

        const QJsonObject renamed = fixture.agents(
            leader, "agents.team_task_update", {{"task_id", id}, {"branch_slug", "login-form"}});
        QVERIFY2(renamed.value("ok").toBool(), qPrintable(agent_test::text(renamed)));
        const QJsonObject page =
            fixture.agents(leader, "agents.team_tasks", {{"task_id", id}}).value("task").toObject();
        QCOMPARE(page.value("branch_slug").toString(), QStringLiteral("login-form"));
        // A leader's update with nothing to rename is still not the leader's.
        QCOMPARE(fixture.agents(leader, "agents.team_task_update", {{"task_id", id}, {"note", "x"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("role_forbidden"));
        QVERIFY(!fixture
                     .agents(developer, "agents.team_task_update",
                             {{"task_id", id}, {"branch_slug", "mine"}})
                     .value("ok")
                     .toBool());
    }
};

QTEST_GUILESS_MAIN(TeamBranchNameTest)
#include "TeamBranchNameTest.moc"
