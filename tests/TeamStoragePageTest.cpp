// ADR 0114 §10 (T-37): the team page's disk warning at its two levels and the removal of a
// done task's worktree, confirmed in a dialog, against a fake daemon.
// Run with QT_QPA_PLATFORM=offscreen.
#include "KetAppearance.h"
#include "KetBanner.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetStyle.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPage.h"
#include "teams/TeamWorktreeDialog.h"

#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QLabel>
#include <QStandardPaths>
#include <QTimer>
#include <QtTest>

#include <functional>

using namespace ketplus;

namespace {

QJsonObject team() {
    return {
        {"id", "7"},
        {"name", "Shop"},
        {"goal", "Checkout"},
        {"repository", "/work/shop"},
        {"integration_branch", "team/shop"},
        {"merge_policy", "leader"},
        {"turn_budget", 60},
        {"automatic_turns", 0},
        {"members",
         QJsonArray{QJsonObject{{"session_id", "lead"}, {"role", "leader"}, {"name", "Lead"}},
                    QJsonObject{{"session_id", "rev"}, {"role", "reviewer"}, {"name", "Review"}}}},
        {"tasks", QJsonArray{QJsonObject{{"id", "T-2"},
                                         {"title", "Quick search"},
                                         {"state", "done"},
                                         {"worktree", "/work/shop-quick-search"},
                                         {"criteria", QJsonArray{}},
                                         {"depends_on", QJsonArray{}},
                                         {"history", QJsonArray{}}}}},
        {"decisions", QJsonArray{}},
        {"revision", 1}};
}

QJsonObject storage(const qint64 free, const QString& level, const bool worktree = true,
                    const bool second = false) {
    QJsonArray worktrees;
    if (worktree)
        worktrees.append(QJsonObject{{"task_id", "T-2"},
                                     {"title", "Quick search"},
                                     {"branch", "team/7/T-2"},
                                     {"path", "/work/shop-quick-search"},
                                     {"size_bytes", 4'200'000'000LL}});
    if (second)
        worktrees.append(QJsonObject{{"task_id", "T-4"},
                                     {"title", "Saved carts"},
                                     {"branch", "team/7/T-4"},
                                     {"path", "/work/shop-saved-carts"},
                                     {"size_bytes", 1'800'000'000LL}});
    return {{"ok", true},
            {"storage", QJsonObject{{"free_bytes", free},
                                    {"level", level},
                                    {"warn_bytes", 20'000'000'000LL},
                                    {"stop_bytes", 12'000'000'000LL},
                                    {"worktrees", worktrees}}}};
}

struct FakeDaemon {
    QList<QPair<QString, QJsonObject>> sent;
    QHash<QString, QJsonObject> answers;
    // T-62: team.worktree.remove's answer for one task, before `answers`.
    QHash<QString, QJsonObject> removals;
    // While set, removals wait in `held` until release().
    bool hold{false};
    QList<std::function<void()>> held;
    TeamDirectory::Transport transport() {
        return [this](const QString& type, const QJsonObject& payload, TeamDirectory::Reply reply) {
            sent.append({type, payload});
            const QString task = payload.value("task_id").toString();
            const QJsonObject answer = type == "team.worktree.remove" && removals.contains(task)
                                           ? removals.value(task)
                                           : answers.value(type, {{"ok", true}});
            auto answerIt = [answer, reply] {
                reply(answer, answer.value("ok").toBool()
                                  ? QString{}
                                  : answer.value("error_message").toString());
            };
            if (hold && type == "team.worktree.remove")
                held.append(answerIt);
            else
                answerIt();
        };
    }
    void release() {
        hold = false;
        while (!held.isEmpty())
            held.takeFirst()();
    }
    QStringList removed() const {
        QStringList ids;
        for (const auto& [type, payload] : sent)
            if (type == "team.worktree.remove")
                ids.append(payload.value("task_id").toString());
        return ids;
    }
    int count(const QString& type) const {
        return static_cast<int>(std::count_if(
            sent.cbegin(), sent.cend(), [&](const auto& entry) { return entry.first == type; }));
    }
};

} // namespace

class TeamStoragePageTest final : public QObject {
    Q_OBJECT

  private:
    FakeDaemon daemon_;
    std::unique_ptr<TeamDirectory> teams_;

    void start(const QJsonObject& answer) {
        daemon_ = {};
        daemon_.answers.insert("team.list", {{"ok", true}, {"teams", QJsonArray{team()}}});
        daemon_.answers.insert("team.get", {{"ok", true}, {"team", team()}});
        daemon_.answers.insert("team.storage.get", answer);
        teams_ = std::make_unique<TeamDirectory>();
        teams_->setTransport(daemon_.transport());
        teams_->setAvailable(true);
    }

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
    }

    void sizesReadInDecimalUnits() {
        QCOMPARE(team_storage::size(18'400'000'000LL), QStringLiteral("18.4 GB"));
        QCOMPARE(team_storage::size(640'000'000LL), QStringLiteral("640.0 MB"));
        QVERIFY(team_storage::warning({}).title.isEmpty());
    }

    // Criterion 1: each level names the free space; enough space shows no banner.
    void theDiskBannerHasTwoLevels_data() {
        QTest::addColumn<qint64>("free");
        QTest::addColumn<QString>("level");
        QTest::addColumn<QString>("title");
        QTest::addColumn<int>("tone");
        QTest::newRow("low") << 18'400'000'000LL << "low" << "Disk space is low: 18.4 GB free"
                             << static_cast<int>(ket::BannerTone::Warning);
        QTest::newRow("stop") << 9'600'000'000LL << "stop" << "Builds should stop: 9.6 GB free"
                              << static_cast<int>(ket::BannerTone::Danger);
        QTest::newRow("enough") << 64'200'000'000LL << "" << "" << -1;
    }
    void theDiskBannerHasTwoLevels() {
        QFETCH(qint64, free);
        QFETCH(QString, level);
        QFETCH(QString, title);
        QFETCH(int, tone);
        start(storage(free, level));
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 900);
        page.show();
        QTRY_VERIFY(daemon_.count("team.storage.get") >= 1);
        QTRY_VERIFY(page.findChild<QWidget*>("teamDoneWorktrees"));
        auto* banner = page.findChild<ket::Banner*>("teamDiskBanner");
        if (title.isEmpty()) {
            QVERIFY(!banner);
            return;
        }
        QVERIFY(banner);
        QCOMPARE(static_cast<int>(banner->tone()), tone);
        QCOMPARE(banner->title(), title);
        QVERIFY(banner->message().contains(level == "stop" ? "12 GB" : "20 GB"));
        // Nothing is removed for the user.
        QCOMPARE(daemon_.count("team.worktree.remove"), 0);
    }

    // Criteria 2 to 4: the offer names the path and size; only a confirmation sends the
    // removal; a refusal stays in the dialog and changes nothing; a removal closes it.
    void aDoneWorktreeIsRemovedOnlyAfterConfirming() {
        start(storage(18'400'000'000LL, "low"));
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 900);
        page.show();
        QTRY_VERIFY(page.findChild<QWidget*>("teamDoneWorktree_T-2"));
        QWidget* row = page.findChild<QWidget*>("teamDoneWorktree_T-2");
        QCOMPARE(row->findChild<QLabel*>("teamDoneWorktreePath")->text(),
                 QStringLiteral("/work/shop-quick-search"));
        QCOMPARE(row->findChild<QLabel*>("teamDoneWorktreeSize")->text(), QStringLiteral("4.2 GB"));
        QCOMPARE(row->findChild<QLabel*>("teamDoneWorktreeTitle")->text(),
                 QStringLiteral("Quick search"));

        const auto open = [&] {
            page.findChild<ket::Button*>("teamDoneWorktreeRemove")->click();
            auto* dialog = page.findChild<ket::Dialog*>("teamWorktreeDialog");
            return dialog;
        };
        QPointer<ket::Dialog> dialog = open();
        QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QLabel*>("teamWorktreePath")->text(),
                 QStringLiteral("/work/shop-quick-search"));
        QVERIFY(dialog->findChild<QLabel*>("teamWorktreeConsequence")->text().contains("4.2 GB"));
        QCOMPARE(daemon_.count("team.worktree.remove"), 0);
        dialog->findChild<ket::Button*>("teamWorktreeCancel")->click();
        QTRY_VERIFY(!dialog);
        QCOMPARE(daemon_.count("team.worktree.remove"), 0);

        daemon_.answers.insert(
            "team.worktree.remove",
            {{"ok", false},
             {"error_code", "worktree_not_removed"},
             {"error_message", "The worktree has uncommitted or untracked changes (2 files)."}});
        dialog = open();
        QVERIFY(dialog);
        auto* remove = dialog->findChild<ket::Button*>("teamWorktreeRemove");
        remove->click();
        QCOMPARE(daemon_.count("team.worktree.remove"), 1);
        QCOMPARE(daemon_.sent.last().second.value("task_id").toString(), QStringLiteral("T-2"));
        QCOMPARE(daemon_.sent.last().second.value("team_id").toString(), QStringLiteral("7"));
        auto* refusal = dialog->findChild<ket::Banner*>("teamWorktreeRefusal");
        QVERIFY(dialog->isVisible());
        QVERIFY(!refusal->isHidden());
        QCOMPARE(refusal->title(), QStringLiteral("Not removed"));
        QVERIFY(refusal->message().contains("2 files"));
        QVERIFY(!remove->isEnabled());
        QVERIFY(page.findChild<QWidget*>("teamDoneWorktree_T-2"));
        dialog->findChild<ket::Button*>("teamWorktreeCancel")->click();
        QTRY_VERIFY(!dialog);

        QJsonObject removed = team();
        QJsonArray tasks = removed.value("tasks").toArray();
        QJsonObject task = tasks.first().toObject();
        task.insert("worktree", "");
        tasks.replace(0, task);
        removed.insert("tasks", tasks);
        removed.insert("revision", 2);
        daemon_.answers.insert("team.worktree.remove", {{"ok", true}, {"team", removed}});
        daemon_.answers.insert("team.storage.get", storage(22'600'000'000LL, "", false));
        dialog = open();
        dialog->findChild<ket::Button*>("teamWorktreeRemove")->click();
        QTRY_VERIFY(!dialog);
        // The task forgot its worktree, so the page measured again: the offer and the banner
        // are gone.
        QTRY_VERIFY(!page.findChild<QWidget*>("teamDoneWorktrees"));
        QVERIFY(!page.findChild<ket::Banner*>("teamDiskBanner"));
    }

    // T-62: no worktree listed, no section and no Remove all….
    void removeAllIsHiddenWithoutWorktrees() {
        start(storage(64'200'000'000LL, "", false));
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 900);
        page.show();
        QTRY_VERIFY(daemon_.count("team.storage.get") >= 1);
        QTest::qWait(50);
        QVERIFY(!page.findChild<QWidget*>("teamDoneWorktrees"));
        QVERIFY(!page.findChild<ket::Button*>("teamDoneWorktreesRemoveAll"));
    }

    // T-62: one confirmation names the count and the total size; Cancel sends nothing; Remove
    // sends team.worktree.remove for each in the list's order, and the page sums it up.
    void removeAllRemovesEveryListedWorktree() {
        start(storage(18'400'000'000LL, "low", true, true));
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 900);
        page.show();
        QTRY_VERIFY(page.findChild<QWidget*>("teamDoneWorktree_T-4"));
        auto* all = page.findChild<ket::Button*>("teamDoneWorktreesRemoveAll");
        QVERIFY(all && all->isVisible() && all->isEnabled());
        all->click();
        QPointer<ket::Dialog> dialog = page.findChild<ket::Dialog*>("teamWorktreeAllDialog");
        QVERIFY(dialog);
        QCOMPARE(dialog->title(), QStringLiteral("Remove 2 worktrees?"));
        const QString consequence =
            dialog->findChild<QLabel*>("teamWorktreeAllConsequence")->text();
        QVERIFY2(consequence.contains("T-2, T-4") && consequence.contains("6.0 GB"),
                 qPrintable(consequence));
        dialog->findChild<ket::Button*>("teamWorktreeAllCancel")->click();
        QTRY_VERIFY(!dialog);
        QCOMPARE(daemon_.count("team.worktree.remove"), 0);

        daemon_.answers.insert("team.storage.get", storage(24'400'000'000LL, "", false));
        page.findChild<ket::Button*>("teamDoneWorktreesRemoveAll")->click();
        dialog = page.findChild<ket::Dialog*>("teamWorktreeAllDialog");
        dialog->findChild<ket::Button*>("teamWorktreeAllRemove")->click();
        QCOMPARE(daemon_.removed(), (QStringList{"T-2", "T-4"}));
        for (const auto& [type, payload] : daemon_.sent)
            if (type == "team.worktree.remove")
                QCOMPARE(payload.value("team_id").toString(), QStringLiteral("7"));
        auto* result = dialog->findChild<ket::Banner*>("teamWorktreeAllResult");
        QVERIFY(!result->isHidden());
        QCOMPARE(result->title(), QStringLiteral("Removed 2 worktrees."));
        QCOMPARE(static_cast<int>(result->tone()), static_cast<int>(ket::BannerTone::Info));
        // Measured again: nothing left to offer, and the page still sums the run up.
        QTRY_VERIFY(!page.findChild<QWidget*>("teamDoneWorktrees"));
        QVERIFY(!page.findChild<ket::Button*>("teamDoneWorktreesRemoveAll"));
        auto* summary = page.findChild<ket::Banner*>("teamDoneWorktreesSummary");
        QVERIFY(summary);
        QCOMPARE(summary->title(), QStringLiteral("Removed 2 worktrees."));
        dialog->findChild<ket::Button*>("teamWorktreeAllCancel")->click();
        QTRY_VERIFY(!dialog);
    }

    // T-62: a refused worktree stays listed with its reason in the daemon's words; the other
    // is still removed; both buttons wait while the run lasts.
    void aRefusedWorktreeStaysWithItsReason() {
        start(storage(18'400'000'000LL, "low", true, true));
        const QString dirty = "The worktree has uncommitted or untracked changes (2 files).";
        daemon_.removals.insert(
            "T-2",
            {{"ok", false}, {"error_code", "worktree_not_removed"}, {"error_message", dirty}});
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 900);
        page.show();
        QTRY_VERIFY(page.findChild<QWidget*>("teamDoneWorktree_T-4"));
        page.findChild<ket::Button*>("teamDoneWorktreesRemoveAll")->click();
        QPointer<ket::Dialog> dialog = page.findChild<ket::Dialog*>("teamWorktreeAllDialog");
        daemon_.hold = true;
        dialog->findChild<ket::Button*>("teamWorktreeAllRemove")->click();
        // The first removal waits: nothing else can start a removal.
        QCOMPARE(daemon_.removed(), QStringList{"T-2"});
        QTRY_VERIFY(!page.findChild<ket::Button*>("teamDoneWorktreesRemoveAll")->isEnabled());
        for (auto* remove : page.findChildren<ket::Button*>("teamDoneWorktreeRemove"))
            QVERIFY(!remove->isEnabled());
        QVERIFY(!dialog->findChild<ket::Button*>("teamWorktreeAllCancel")->isEnabled());

        daemon_.answers.insert("team.storage.get", storage(22'600'000'000LL, "", true, false));
        daemon_.release();
        QTRY_COMPARE(daemon_.removed(), (QStringList{"T-2", "T-4"}));
        daemon_.release();
        auto* result = dialog->findChild<ket::Banner*>("teamWorktreeAllResult");
        QTRY_VERIFY(!result->isHidden());
        QCOMPARE(result->title(), QStringLiteral("Removed 1 worktree. Kept 1 with its reason."));
        QVERIFY(result->message().contains(QStringLiteral("T-2: ") + dirty));
        QCOMPARE(static_cast<int>(result->tone()), static_cast<int>(ket::BannerTone::Warning));

        // The page lists T-2 again from the daemon's measure, with the reason, and sums up.
        QTRY_VERIFY(!page.findChild<QWidget*>("teamDoneWorktree_T-4"));
        QWidget* kept = page.findChild<QWidget*>("teamDoneWorktree_T-2");
        QVERIFY(kept);
        QTRY_VERIFY(kept->findChild<QLabel*>("teamDoneWorktreeReason"));
        QCOMPARE(kept->findChild<QLabel*>("teamDoneWorktreeReason")->text(), dirty);
        auto* summary = page.findChild<ket::Banner*>("teamDoneWorktreesSummary");
        QVERIFY(summary);
        QCOMPARE(summary->title(), QStringLiteral("Removed 1 worktree. Kept 1 with its reason."));
        QVERIFY(page.findChild<ket::Button*>("teamDoneWorktreesRemoveAll")->isEnabled());
        dialog->findChild<ket::Button*>("teamWorktreeAllCancel")->click();
        QTRY_VERIFY(!dialog);

        // T-2 was removed since, by hand: "Kept 1" is not true any more, so the summary goes.
        daemon_.answers.insert("team.storage.get", storage(25'000'000'000LL, "", false, false));
        for (auto* timer : page.findChildren<QTimer*>())
            if (timer->interval() == 60 * 1000)
                timer->setInterval(1);
        QTRY_VERIFY(!page.findChild<QWidget*>("teamDoneWorktree_T-2"));
        QTRY_VERIFY(!page.findChild<ket::Banner*>("teamDoneWorktreesSummary"));
        QTRY_VERIFY(!page.findChild<QWidget*>("teamDoneWorktreesHeading"));
    }

    // With KETPLUS_TEAM_STORAGE_CAPTURE set to a folder, saves the page at both levels and the
    // dialog, plain and refused, in light and dark, as review evidence beside the mock.
    void captureForReview() {
        const QString folder = qEnvironmentVariable("KETPLUS_TEAM_STORAGE_CAPTURE");
        if (folder.isEmpty())
            QSKIP("KETPLUS_TEAM_STORAGE_CAPTURE is not set.");
        auto& appearance = ket::Appearance::instance();
        const auto save = [&](QWidget& page, const QString& name) {
            for (const auto& [mode, suffix] : {std::pair(ket::Appearance::Mode::Light, "light"),
                                               std::pair(ket::Appearance::Mode::Dark, "dark")}) {
                appearance.setMode(mode);
                QTest::qWait(200);
                QVERIFY(page.grab().save(QDir(folder).filePath(name + "-" + suffix + ".png")));
            }
            appearance.setMode(ket::Appearance::Mode::Light);
        };
        for (const auto& [free, level] : {std::pair(18'400'000'000LL, QStringLiteral("low")),
                                          std::pair(9'600'000'000LL, QStringLiteral("stop"))}) {
            start(storage(free, level));
            TeamPage page(teams_.get(), "7", "/work/shop");
            page.resize(1280, 900);
            page.show();
            QTRY_VERIFY(page.findChild<QWidget*>("teamDoneWorktrees"));
            save(page, "disk-" + level);
        }
        start(storage(18'400'000'000LL, "low"));
        daemon_.answers.insert(
            "team.worktree.remove",
            {{"ok", false},
             {"error_code", "worktree_not_removed"},
             {"error_message", "The worktree has uncommitted or untracked changes (2 files). "
                               "Commit or discard them, then remove it again."}});
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1280, 900);
        page.show();
        QTRY_VERIFY(page.findChild<QWidget*>("teamDoneWorktrees"));
        page.findChild<ket::Button*>("teamDoneWorktreeRemove")->click();
        auto* dialog = page.findChild<ket::Dialog*>("teamWorktreeDialog");
        QVERIFY(dialog);
        save(page, "remove-worktree");
        dialog->findChild<ket::Button*>("teamWorktreeRemove")->click();
        save(page, "remove-worktree-refused");
        dialog->findChild<ket::Button*>("teamWorktreeCancel")->click();
        QTRY_VERIFY(!page.findChild<ket::Dialog*>("teamWorktreeDialog"));
        daemon_.answers.insert(
            "team.worktree.remove",
            {{"ok", true},
             {"team", team()},
             {"note", "“Dev 1” (Wait for startup to finish.) could not be moved to the main "
                      "checkout and still points at the removed folder. Open it on another folder "
                      "before its next turn."}});
        page.findChild<ket::Button*>("teamDoneWorktreeRemove")->click();
        dialog = page.findChild<ket::Dialog*>("teamWorktreeDialog");
        QVERIFY(dialog);
        dialog->findChild<ket::Button*>("teamWorktreeRemove")->click();
        save(page, "remove-worktree-note");
    }

    // Review finding: a removal after which a conversation could not be moved out of the
    // folder keeps the dialog open and says so in words; nothing more can be sent from it.
    void aConversationThatStayedIsNamed() {
        start(storage(18'400'000'000LL, "low"));
        daemon_.answers.insert(
            "team.worktree.remove",
            {{"ok", true},
             {"team", team()},
             {"note", "“Dev 1” (Wait for startup to finish.) could not be moved to the main "
                      "checkout and still points at the removed folder. Open it on another folder "
                      "before its next turn."}});
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 900);
        page.show();
        QTRY_VERIFY(page.findChild<QWidget*>("teamDoneWorktrees"));
        page.findChild<ket::Button*>("teamDoneWorktreeRemove")->click();
        QPointer<ket::Dialog> dialog = page.findChild<ket::Dialog*>("teamWorktreeDialog");
        QVERIFY(dialog);
        auto* remove = dialog->findChild<ket::Button*>("teamWorktreeRemove");
        remove->click();
        QCOMPARE(daemon_.count("team.worktree.remove"), 1);
        QVERIFY(dialog->isVisible());
        auto* note = dialog->findChild<ket::Banner*>("teamWorktreeNote");
        QVERIFY(!note->isHidden());
        QVERIFY(note->message().startsWith("“Dev 1”"));
        QVERIFY(dialog->findChild<ket::Banner*>("teamWorktreeRefusal")->isHidden());
        QVERIFY(remove->isHidden());
        auto* close = dialog->findChild<ket::Button*>("teamWorktreeCancel");
        QCOMPARE(close->text(), QStringLiteral("Close"));
        close->click();
        QTRY_VERIFY(!dialog);
    }

    // An older daemon refuses team.storage.get: the page shows neither part.
    void aDaemonWithoutStorageShowsNothing() {
        start({{"ok", false}, {"error_code", "unknown_operation"}, {"error_message", "No."}});
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 900);
        page.show();
        QTRY_VERIFY(daemon_.count("team.storage.get") >= 1);
        QVERIFY(page.findChild<QLabel*>("teamPageName"));
        QVERIFY(!page.findChild<ket::Banner*>("teamDiskBanner"));
        QVERIFY(!page.findChild<QWidget*>("teamDoneWorktrees"));
    }
};

QTEST_MAIN(TeamStoragePageTest)
#include "TeamStoragePageTest.moc"
