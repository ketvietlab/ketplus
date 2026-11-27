// ADR 0114 §4: a lane is a developer's name for its work, renamed by the leader. T-10 removed
// lane paths and the check outside a lane; a team stored with them still loads. The coordinator
// cases use a fake runtime; the diff case a temporary Git repository. No provider is started.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "protocol/DaemonProtocol.h"
#include "runtime/TeamGit.h"

#include <QDir>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>

using namespace ketplus::daemon;
using namespace team_test;

namespace {

void git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", arguments);
    QVERIFY(process.waitForFinished(15000));
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
}

QString revision(const QString& directory) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", {"rev-parse", "HEAD"});
    process.waitForFinished(15000);
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

void commitAll(const QString& directory, const QString& message) {
    git(directory, {"add", "-A"});
    git(directory,
        {"-c", "user.email=t@example.com", "-c", "user.name=T", "commit", "-q", "-m", message});
}

void write(const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x\n");
}

} // namespace

class TeamLaneTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};
    QString task_;

    const Team& team() { return *teams_->team(team_); }
    const Task& task() { return *team().task(task_); }
    QList<TeamUpdateRecord> updates(const QString& kind) {
        QList<TeamUpdateRecord> result;
        for (const TeamUpdateRecord& update : journal_->teamUpdates(team_, false, 200))
            if (update.kind == kind)
                result.append(update);
        return result;
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        teams_.reset();
        journal_.reset();
        QFile::remove(directory_.filePath("journal.sqlite3"));
        host_ = {};
        for (const QString& id : {"lead", "dev1", "dev2"})
            host_.add(id);
        host_.add("rev", "claude");
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(60 * 60 * 1000);
        teams_->load();
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
        QVERIFY(teams_->createTask(team_, {.title = "Cart", .lane = "Web"}, "lead", &task_).ok());
        QVERIFY(teams_->assignTask(team_, task_, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, task_, state::inProgress, "dev1").ok());
    }

    void aLaneRenameIsLoggedOnItsOpenTasks() {
        QVERIFY(
            teams_->updateMember(team_, "dev1", "Storefront", "lead", "the cart and more").ok());
        QCOMPARE(team().member("dev1")->lane, QStringLiteral("Storefront"));
        const auto changed = updates("lane_changed");
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.first().actor, QStringLiteral("lead"));
        QVERIFY(changed.first().text.contains("the cart and more"));
        QCOMPARE(changed.first().data.value("old_lane").toString(), QStringLiteral("Web"));
        QCOMPARE(changed.first().data.value("lane").toString(), QStringLiteral("Storefront"));
        QVERIFY(!changed.first().data.contains("paths"));
        bool logged = false;
        for (const HistoryEntry& entry : task().history)
            logged = logged || (entry.kind == "lane" && entry.actor == "lead" &&
                                entry.text.contains("the cart and more"));
        QVERIFY(logged);
        // No other member changes, and a developer's turn raises no lane flag.
        QCOMPARE(team().member("dev2")->lane, QStringLiteral("Api"));
        teams_->memberTurnEnded("dev1");
        QVERIFY(!toJson(task(), false).contains("outside_lane"));
    }

    void anUnchangedLaneIsANoOpAndAnEmptyOneIsRefused() {
        QVERIFY(teams_->updateMember(team_, "dev1", "Web", "lead").ok());
        QVERIFY(updates("lane_changed").isEmpty());
        QCOMPARE(teams_->updateMember(team_, "dev1", "  ", "lead").error.value_or(TeamError{}).code,
                 QStringLiteral("invalid_payload"));
        QCOMPARE(
            teams_->updateMember(team_, "rev", "Docs", "lead").error.value_or(TeamError{}).code,
            QStringLiteral("not_member"));
        QCOMPARE(team().member("dev1")->lane, QStringLiteral("Web"));
    }

    // Two developers may share a lane name: a lane no longer owns files.
    void twoDevelopersMayShareALane() {
        QVERIFY(teams_->updateMember(team_, "dev2", "Web", "lead", "help on the cart").ok());
        QCOMPARE(team().member("dev2")->lane, QStringLiteral("Web"));
    }

    // A team stored before T-10 still loads: its lane paths, shared paths and the old lane
    // flags on tasks are read and dropped, and are never written again.
    void aTeamStoredWithLanePathsStillLoads() {
        QJsonObject stored = toJson(team());
        QJsonArray members = stored.value("members").toArray();
        QJsonObject developer = members.at(1).toObject();
        developer.insert("paths", QJsonArray{"web", "docs"});
        members.replace(1, developer);
        stored.insert("members", members);
        stored.insert("shared_paths", QJsonArray{"tests"});
        QJsonArray tasks = stored.value("tasks").toArray();
        QJsonObject old = tasks.at(0).toObject();
        old.insert("outside_lane", QJsonArray{"api/routes.cpp"});
        old.insert("allowed_paths", QJsonArray{"tests/fixtures"});
        tasks.replace(0, old);
        stored.insert("tasks", tasks);
        const Team loaded = teamFromJson(stored);
        QCOMPARE(loaded.members.size(), team().members.size());
        QCOMPARE(loaded.member("dev1")->lane, QStringLiteral("Web"));
        QCOMPARE(loaded.tasks.size(), 1);
        QCOMPARE(loaded.tasks.first().title, QStringLiteral("Cart"));
        const QJsonObject again = toJson(loaded);
        QVERIFY(!again.contains("shared_paths"));
        QVERIFY(!again.value("members").toArray().at(1).toObject().contains("paths"));
        const QJsonObject task = again.value("tasks").toArray().at(0).toObject();
        QVERIFY(!task.contains("outside_lane"));
        QVERIFY(!task.contains("allowed_paths"));
    }

    void theTaskDiffHoldsOnlyTheBranchsOwnCommits() {
        QTemporaryDir root;
        const QString repository = QDir(root.path()).filePath("shop");
        QVERIFY(QDir().mkpath(repository));
        git(repository, {"init", "-q", "-b", "main"});
        write(QDir(repository).filePath("README"));
        commitAll(repository, "start");
        const QString base = revision(repository);
        const QString worktree = QDir(root.path()).filePath("task");
        git(repository, {"worktree", "add", "-q", "-b", "team/t-1", worktree});
        write(QDir(repository).filePath("api/routes.txt"));
        commitAll(repository, "someone else");
        write(QDir(worktree).filePath("web/cart.txt"));
        // A path git would quote: it is read as it is spelled (#484).
        write(QDir(worktree).filePath(QString::fromUtf8("web/gi\xe1\xbb\x8f.txt")));
        commitAll(worktree, "mine");
        git(worktree, {"-c", "user.email=t@example.com", "-c", "user.name=T", "merge", "-q",
                       "--no-edit", "main"});
        // Staged but not committed: not part of the diff yet.
        write(QDir(worktree).filePath("web/pending.txt"));
        git(worktree, {"add", "web/pending.txt"});

        Team team;
        team.repository = repository;
        Task task;
        task.worktree = worktree;
        task.branch = "team/t-1";
        task.baseCommit = base;
        const QString quoted = QString::fromUtf8("web/gi\xe1\xbb\x8f.txt");
        // The diff is from where the branch left the integration branch: a merge of it brings
        // nothing of another task, and uncommitted work is not yet part of it.
        team.integrationBranch = "main";
        const auto diff = team_git::taskDiff(team, task);
        QVERIFY(diff);
        QCOMPARE(diff->files, (QStringList{"web/cart.txt", quoted}));
    }

    // A text bounded when it was written is the same when the document is read again, and
    // the note still counts the characters of the original. No surrogate pair is cut in half.
    void aBoundedTextIsUnchangedWhenReadAgain() {
        const QString once = boundedText(QString(5000, u'h'));
        QVERIFY(once.size() <= maximumHistoryText);
        QVERIFY2(once.endsWith(QStringLiteral("(3040 more characters)")),
                 qPrintable(once.right(40)));
        QCOMPARE(boundedText(once), once);
        QCOMPARE(bounded({.text = once}).text, once);
        const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
        const QString cut = boundedText(QString(1959, u'h') + emoji + QString(100, u'h'));
        QVERIFY(!cut.contains(emoji.at(0)));
        QCOMPARE(cut.indexOf(QChar(0x2026)), 1959);
    }
};

QTEST_GUILESS_MAIN(TeamLaneTest)
#include "TeamLaneTest.moc"
