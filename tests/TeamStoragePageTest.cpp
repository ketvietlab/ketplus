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

// T-23: the daemon's cleanup state beside the storage, and the rows it holds.
QJsonObject cleanupRunning(const qint64 run = 1) {
    return {{"run", run},
            {"active", QJsonArray{QJsonObject{{"task_id", "T-2"}, {"state", "removing"}},
                                  QJsonObject{{"task_id", "T-4"}, {"state", "queued"}}}},
            {"last", QJsonValue::Null}};
}

QJsonObject cleanupFinished(const qint64 run, const QJsonArray& removed, const QJsonArray& kept,
                            const qint64 freed, const bool exact = true) {
    return {{"run", run},
            {"active", QJsonArray{}},
            {"last", QJsonObject{{"run", run},
                                 {"removed", removed},
                                 {"kept", kept},
                                 {"notes", QJsonArray{}},
                                 {"freed_bytes", freed},
                                 {"freed_exact", exact},
                                 {"finished_at", "2026-11-23T05:00:00.000Z"}}}};
}

QJsonObject storage(const qint64 free, const QString& level, const bool worktree = true,
                    const bool second = false, const QJsonObject& cleanup = {},
                    const QHash<QString, QString>& removal = {}) {
    QJsonArray worktrees;
    const auto add = [&](QJsonObject entry) {
        if (removal.contains(entry.value("task_id").toString()))
            entry.insert("removal", removal.value(entry.value("task_id").toString()));
        worktrees.append(entry);
    };
    if (worktree)
        add({{"task_id", "T-2"},
             {"title", "Quick search"},
             {"branch", "team/7/T-2"},
             {"path", "/work/shop-quick-search"},
             {"size_bytes", 4'200'000'000LL}});
    if (second)
        add({{"task_id", "T-4"},
             {"title", "Saved carts"},
             {"branch", "team/7/T-4"},
             {"path", "/work/shop-saved-carts"},
             {"size_bytes", 1'800'000'000LL}});
    QJsonObject body{{"free_bytes", free},
                     {"level", level},
                     {"warn_bytes", 20'000'000'000LL},
                     {"stop_bytes", 12'000'000'000LL},
                     {"worktrees", worktrees}};
    if (!cleanup.isEmpty())
        body.insert("cleanup", cleanup);
    return {{"ok", true}, {"storage", body}};
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

    // T-23 criteria 1 and 2: with a daemon that cleans up in the background, Remove all… sends
    // one request that is answered at once, the dialog closes, and the page shows the worktrees
    // the daemon holds locked, in words, with nothing to press twice.
    void aBackgroundCleanupClosesTheDialogAndLocksTheRows() {
        start(storage(18'400'000'000LL, "low", true, true));
        teams_->setCleanupAvailable(true);
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 900);
        page.show();
        QTRY_VERIFY(page.findChild<QWidget*>("teamDoneWorktree_T-4"));
        page.findChild<ket::Button*>("teamDoneWorktreesRemoveAll")->click();
        QPointer<ket::Dialog> dialog = page.findChild<ket::Dialog*>("teamWorktreeAllDialog");
        QVERIFY(dialog);
        // The daemon will report the run when the page reads again.
        daemon_.answers.insert("team.storage.get",
                               storage(18'400'000'000LL, "low", true, true, cleanupRunning(),
                                       {{"T-2", "removing"}, {"T-4", "queued"}}));
        dialog->findChild<ket::Button*>("teamWorktreeAllRemove")->click();
        QCOMPARE(daemon_.count("team.worktree.cleanup"), 1);
        QCOMPARE(daemon_.count("team.worktree.remove"), 0);
        for (const auto& [type, payload] : daemon_.sent)
            if (type == "team.worktree.cleanup") {
                QCOMPARE(payload.value("team_id").toString(), QStringLiteral("7"));
                QCOMPARE(payload.value("task_ids").toArray(), (QJsonArray{"T-2", "T-4"}));
            }
        QTRY_VERIFY(!dialog);

        // Both rows are locked, with a word and an icon; nothing here moves by itself.
        QTRY_VERIFY(page.findChild<QWidget*>("teamCleanupRunning"));
        QWidget* removing = page.findChild<QWidget*>("teamDoneWorktree_T-2");
        QWidget* queued = page.findChild<QWidget*>("teamDoneWorktree_T-4");
        auto* removingButton = removing->findChild<ket::Button*>("teamDoneWorktreeRemove");
        auto* queuedButton = queued->findChild<ket::Button*>("teamDoneWorktreeRemove");
        QCOMPARE(removingButton->text(), QStringLiteral("Removing…"));
        QCOMPARE(queuedButton->text(), QStringLiteral("Queued"));
        QVERIFY(!removingButton->isEnabled() && !queuedButton->isEnabled());
        QVERIFY(!removingButton->icon().isNull() && !queuedButton->icon().isNull());
        QVERIFY(removingButton->accessibleName().contains("being removed"));
        QVERIFY(queuedButton->accessibleName().contains("waits to be removed"));
        auto* running = page.findChild<ket::Banner*>("teamCleanupRunning");
        QCOMPARE(running->title(), QStringLiteral("Removing 2 worktrees in the background"));
        // Remove all… cannot run twice, and the rest of the page is not locked.
        auto* all = page.findChild<ket::Button*>("teamDoneWorktreesRemoveAll");
        QVERIFY(!all->isEnabled());
        all->click();
        removingButton->click();
        QCOMPARE(daemon_.count("team.worktree.cleanup"), 1);
        QVERIFY(!page.findChild<ket::Dialog*>("teamWorktreeAllDialog"));
        QVERIFY(page.isEnabled());
        QVERIFY(page.findChild<QLabel*>("teamPageName")->isVisible());
    }

    // T-23 criterion 3: the daemon says it moved, the page reads again, and what the run did
    // stays in a summary: how many, how much, and what was kept with its reason. Closing it
    // is the user's; the next run shows its own.
    void theSummaryNamesWhatTheRunDidAndStaysUntilClosed() {
        start(storage(18'400'000'000LL, "low", true, true, cleanupRunning(1),
                      {{"T-2", "removing"}, {"T-4", "queued"}}));
        teams_->setCleanupAvailable(true);
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 900);
        page.show();
        QTRY_VERIFY(page.findChild<QWidget*>("teamCleanupRunning"));
        const int reads = daemon_.count("team.storage.get");

        // A team that is not this page's own changes nothing here.
        teams_->applyEvent("team.storage.changed", {{"team_id", "8"}});
        QTest::qWait(30);
        QCOMPARE(daemon_.count("team.storage.get"), reads);

        const QJsonArray kept{QJsonObject{
            {"task_id", "T-4"},
            {"reason", "The worktree has uncommitted or untracked changes (2 files)."}}};
        daemon_.answers.insert(
            "team.storage.get",
            storage(
                30'000'000'000LL, "", false, true,
                cleanupFinished(
                    1, QJsonArray{QJsonObject{{"task_id", "T-2"}, {"size_bytes", 4'200'000'000LL}}},
                    kept, 4'200'000'000LL)));
        teams_->applyEvent("team.storage.changed", {{"team_id", "7"}});
        QTRY_VERIFY(!page.findChild<QWidget*>("teamCleanupRunning"));
        auto* summary = page.findChild<ket::Banner*>("teamDoneWorktreesSummary");
        QVERIFY(summary);
        QCOMPARE(summary->title(),
                 QStringLiteral("Removed 1 worktree, freeing 4.2 GB. Kept 1 with its reason."));
        QVERIFY(summary->message().contains("T-4: The worktree has uncommitted or untracked"));
        QCOMPARE(static_cast<int>(summary->tone()), static_cast<int>(ket::BannerTone::Warning));
        // The kept worktree is unlocked again and can be asked for again.
        auto* button = page.findChild<QWidget*>("teamDoneWorktree_T-4")
                           ->findChild<ket::Button*>("teamDoneWorktreeRemove");
        QCOMPARE(button->text(), QStringLiteral("Remove…"));
        QVERIFY(button->isEnabled());
        QVERIFY(page.findChild<ket::Button*>("teamDoneWorktreesRemoveAll")->isEnabled());

        // Closed, it stays closed for that run, whenever the page reads again.
        summary->action()->click();
        QTRY_VERIFY(!page.findChild<ket::Banner*>("teamDoneWorktreesSummary"));
        teams_->applyEvent("team.storage.changed", {{"team_id", "7"}});
        QTest::qWait(50);
        QVERIFY(!page.findChild<ket::Banner*>("teamDoneWorktreesSummary"));
        // The next run has its own.
        daemon_.answers.insert(
            "team.storage.get",
            storage(30'000'000'000LL, "", false, false,
                    cleanupFinished(
                        2,
                        QJsonArray{QJsonObject{{"task_id", "T-4"}, {"size_bytes", QJsonValue()}}},
                        QJsonArray{}, 0, false)));
        teams_->applyEvent("team.storage.changed", {{"team_id", "7"}});
        QTRY_VERIFY(page.findChild<ket::Banner*>("teamDoneWorktreesSummary"));
        QCOMPARE(page.findChild<ket::Banner*>("teamDoneWorktreesSummary")->title(),
                 QStringLiteral("Removed 1 worktree."));
        QCOMPARE(static_cast<int>(page.findChild<ket::Banner*>("teamDoneWorktreesSummary")->tone()),
                 static_cast<int>(ket::BannerTone::Info));
        QVERIFY(page.findChild<QWidget*>("teamDoneWorktreesHeading"));
    }

    // T-23 criterion 3: the state is the daemon's, so a page opened again, or for another team
    // and back, shows the locked rows and, afterwards, the summary.
    void theStateSurvivesOpeningThePageAgain() {
        start(storage(18'400'000'000LL, "low", true, true, cleanupRunning(1),
                      {{"T-2", "removing"}, {"T-4", "queued"}}));
        teams_->setCleanupAvailable(true);
        {
            TeamPage first(teams_.get(), "7", "/work/shop");
            first.resize(1024, 900);
            first.show();
            QTRY_VERIFY(first.findChild<QWidget*>("teamCleanupRunning"));
        }
        TeamPage again(teams_.get(), "7", "/work/shop");
        again.resize(1024, 900);
        again.show();
        QTRY_VERIFY(again.findChild<QWidget*>("teamCleanupRunning"));
        QCOMPARE(again.findChild<QWidget*>("teamDoneWorktree_T-2")
                     ->findChild<ket::Button*>("teamDoneWorktreeRemove")
                     ->text(),
                 QStringLiteral("Removing…"));
        QVERIFY(!again.findChild<ket::Button*>("teamDoneWorktreesRemoveAll")->isEnabled());

        // It ended while no page was open: the page opened then shows the summary.
        daemon_.answers.insert(
            "team.storage.get",
            storage(
                30'000'000'000LL, "", false, false,
                cleanupFinished(
                    1,
                    QJsonArray{QJsonObject{{"task_id", "T-2"}, {"size_bytes", 4'200'000'000LL}},
                               QJsonObject{{"task_id", "T-4"}, {"size_bytes", 1'800'000'000LL}}},
                    QJsonArray{}, 6'000'000'000LL)));
        TeamPage later(teams_.get(), "7", "/work/shop");
        later.resize(1024, 900);
        later.show();
        QTRY_VERIFY(later.findChild<ket::Banner*>("teamDoneWorktreesSummary"));
        QCOMPARE(later.findChild<ket::Banner*>("teamDoneWorktreesSummary")->title(),
                 QStringLiteral("Removed 2 worktrees, freeing 6.0 GB."));
        QVERIFY(!later.findChild<QWidget*>("teamCleanupRunning"));
    }

    // T-23: Remove… for one worktree takes the same road; a request the daemon refuses stays
    // in the dialog in its words, and nothing locks.
    void oneRemovalInTheBackgroundClosesTheDialog() {
        start(storage(18'400'000'000LL, "low"));
        teams_->setCleanupAvailable(true);
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 900);
        page.show();
        QTRY_VERIFY(page.findChild<QWidget*>("teamDoneWorktree_T-2"));
        daemon_.answers.insert("team.worktree.cleanup",
                               {{"ok", false},
                                {"error_code", "team_not_found"},
                                {"error_message", "The team no longer exists."}});
        page.findChild<ket::Button*>("teamDoneWorktreeRemove")->click();
        QPointer<ket::Dialog> dialog = page.findChild<ket::Dialog*>("teamWorktreeDialog");
        QVERIFY(dialog);
        dialog->findChild<ket::Button*>("teamWorktreeRemove")->click();
        QVERIFY(dialog->isVisible());
        auto* refusal = dialog->findChild<ket::Banner*>("teamWorktreeRefusal");
        QVERIFY(!refusal->isHidden());
        QVERIFY(refusal->message().contains("no longer exists"));
        QCOMPARE(daemon_.count("team.worktree.remove"), 0);
        dialog->findChild<ket::Button*>("teamWorktreeCancel")->click();
        QTRY_VERIFY(!dialog);

        daemon_.answers.insert("team.worktree.cleanup",
                               {{"ok", true}, {"cleanup", cleanupRunning()}});
        daemon_.answers.insert(
            "team.storage.get",
            storage(18'400'000'000LL, "low", true, false, cleanupRunning(), {{"T-2", "removing"}}));
        page.findChild<ket::Button*>("teamDoneWorktreeRemove")->click();
        dialog = page.findChild<ket::Dialog*>("teamWorktreeDialog");
        dialog->findChild<ket::Button*>("teamWorktreeRemove")->click();
        QTRY_VERIFY(!dialog);
        QCOMPARE(daemon_.count("team.worktree.cleanup"), 2);
        QJsonObject lastCleanup;
        for (const auto& [type, payload] : daemon_.sent)
            if (type == "team.worktree.cleanup")
                lastCleanup = payload;
        QCOMPARE(lastCleanup.value("task_ids").toArray(), QJsonArray{"T-2"});
        QTRY_COMPARE(page.findChild<QWidget*>("teamDoneWorktree_T-2")
                         ->findChild<ket::Button*>("teamDoneWorktreeRemove")
                         ->text(),
                     QStringLiteral("Removing…"));
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
        dialog->findChild<ket::Button*>("teamWorktreeCancel")->click();

        // T-23: the background cleanup, while it runs and once it ended.
        start(storage(18'400'000'000LL, "low", true, true, cleanupRunning(),
                      {{"T-2", "removing"}, {"T-4", "queued"}}));
        teams_->setCleanupAvailable(true);
        TeamPage running(teams_.get(), "7", "/work/shop");
        running.resize(1280, 900);
        running.show();
        QTRY_VERIFY(running.findChild<QWidget*>("teamCleanupRunning"));
        save(running, "cleanup-running");
        const QJsonArray kept{QJsonObject{
            {"task_id", "T-4"},
            {"reason", "The worktree has uncommitted or untracked changes (2 files)."}}};
        start(storage(
            30'000'000'000LL, "", false, true,
            cleanupFinished(
                1, QJsonArray{QJsonObject{{"task_id", "T-2"}, {"size_bytes", 4'200'000'000LL}}},
                kept, 4'200'000'000LL)));
        teams_->setCleanupAvailable(true);
        TeamPage finished(teams_.get(), "7", "/work/shop");
        finished.resize(1280, 900);
        finished.show();
        QTRY_VERIFY(finished.findChild<ket::Banner*>("teamDoneWorktreesSummary"));
        save(finished, "cleanup-done");
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
