#include "app/ExecutionWorktreeDialog.h"
#include "KetDialog.h"
#include "KetToast.h"
#include "ui/Theme.h"
#include <QAbstractButton>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QtTest>

#include <utility>

class ExecutionWorktreeDialogTest final : public QObject {
    Q_OBJECT
  private slots:
    void scrollSearchAndSelectionSwitchExecutionAfterAcknowledgement() {
        QWidget parent; // the picker is a card inside this window
        parent.resize(900, 600);
        parent.show();
        QString selected;
        QJsonObject switched;
        ketplus::showExecutionWorktreeDialog(
            &parent,
            [&](const QString& type, const QJsonObject& payload, auto done) {
                if (type == "session.worktree.switch") {
                    switched = payload;
                    done({{"ok", true},
                          {"worktree_path", payload.value("worktree_path")},
                          {"context_revision", 8}},
                         {});
                    return;
                }
                QJsonArray entries;
                const int offset = payload.value("offset").toInt();
                for (int i = offset; i < offset + 20; ++i)
                    entries.append(QJsonObject{{"branch", QString("feature/%1").arg(i)},
                                               {"worktree_path", QString("/repo-%1").arg(i)},
                                               {"model_touched", i < 2}});
                done({{"worktrees", entries},
                      {"total", 1248},
                      {"worktree_path", "/current"},
                      {"context_revision", 7},
                      {"switch_supported", true}},
                     {});
            },
            [&](const QString& branch) { selected = branch; });
        auto* dialog = parent.findChild<ket::Dialog*>();
        QVERIFY(dialog);
        auto* list = dialog->findChild<QListWidget*>("executionWorktreeList");
        QVERIFY(list);
        QCOMPARE(list->count(), 20);
        QVERIFY(list->item(0)->text().contains("✦"));
        QVERIFY(!list->item(2)->text().contains("✦"));
        QTest::qWait(30);
        list->verticalScrollBar()->setValue(list->verticalScrollBar()->maximum());
        QTRY_COMPARE(list->count(), 40);
        QVERIFY(list->item(20)->text().startsWith("feature/20"));
        auto* search = dialog->findChild<QLineEdit*>("worktreeSearch");
        search->setText("feature");
        QTRY_VERIFY(list->isEnabled());
        QVERIFY(list->item(0)->text().startsWith("feature/0"));
        QTRY_COMPARE(list->verticalScrollBar()->value(), 0);
        // Choosing a worktree switches nothing; Use this repository does, then closes.
        auto* use = dialog->findChild<QAbstractButton*>("useRepositoryButton");
        QVERIFY(use);
        list->setCurrentItem(list->item(1));
        QVERIFY(switched.isEmpty());
        QVERIFY(use->isEnabled());
        const QPointer<ket::Dialog> open(dialog);
        use->click();
        QCOMPARE(switched.value("expected_context_revision").toInt(), 7);
        QCOMPARE(switched.value("worktree_path").toString(), "/repo-1");
        QCOMPARE(selected, "/repo-1");
        QTRY_VERIFY(open.isNull() || !open->isVisible());
    }
    void repositoryListsItsWorktreesThenUseSwitchesAfterAck() {
        // As in the app, the theme manager exists before any dialog, so it restyles the
        // application before the kit's widgets refresh their recipes.
        ketplus::ThemeManager theme(*qApp);
        QWidget parent; // the picker is a card inside this window
        parent.resize(900, 600);
        parent.show();
        QString selected;
        QJsonObject mutation;
        QList<QJsonObject> listed;
        std::function<void(const QJsonObject&, const QString&)> acknowledgement;
        QString current = "/workspace/a";
        bool created = false;
        bool olderService = true;
        ketplus::showExecutionWorktreeDialog(
            &parent,
            [&](const QString& type, const QJsonObject& payload, auto done) {
                if (type == "session.worktree.switch") {
                    mutation = payload;
                    acknowledgement = done;
                    return;
                }
                listed.append(payload);
                const auto repository = payload.value("repository_path").toString("/workspace/a");
                QJsonArray entries{
                    QJsonObject{{"branch", "main"}, {"worktree_path", repository}, {"main", true}},
                    QJsonObject{{"branch", "feat/pilot"},
                                {"worktree_path", repository + "-pilot"}}};
                if (payload.contains("repository_path") && olderService) {
                    done({{"ok", true}, {"worktree_path", current}, {"worktrees", QJsonArray{}}},
                         {});
                    return;
                }
                done({{"ok", true},
                      {"repository_path", repository},
                      {"worktree_path", current},
                      {"context_revision", 4},
                      {"switch_supported", true},
                      {"switch_available", true},
                      {"total", 2},
                      {"worktrees", entries}},
                     {});
            },
            [&](const QString& path) { selected = path; }, current,
            {"/workspace/b", "/workspace/a"}, "/workspace/a", [&] { created = true; });
        auto* dialog = parent.findChild<ket::Dialog*>("executionWorktreeDialog");
        QVERIFY(dialog);
        // An in-window card as the mock's: its title in the header, the actions in its footer.
        QCOMPARE(dialog->title(), QStringLiteral("Repository & worktree"));
        QVERIFY(!dialog->isWindow());
        QCOMPARE(dialog->geometry(), parent.rect());
        QCOMPARE(dialog->card()->width(), 800);
        auto* repos = dialog->findChild<QListWidget*>("executionRepositoryList");
        auto* list = dialog->findChild<QListWidget*>("executionWorktreeList");
        auto* use = dialog->findChild<QAbstractButton*>("useRepositoryButton");
        QVERIFY(repos && list && use);
        QVERIFY(dialog->card()->isAncestorOf(use));
        QVERIFY(!dialog->body()->isAncestorOf(use));
        // The current repository is listed first with its worktrees; the current one is chosen.
        QCOMPARE(repos->item(0)->data(Qt::UserRole).toString(), current);
        QVERIFY(!listed.constFirst().contains("repository_path"));
        QCOMPARE(list->count(), 2);
        QVERIFY(list->item(0)->text().startsWith("main  · Main"));
        QVERIFY(list->currentItem() == list->item(0));

        // Selecting another repository lists its worktrees and switches nothing.
        auto* search = dialog->findChild<QLineEdit*>("repositorySearch");
        search->setText("b");
        QCOMPARE(repos->count(), 1);
        emit repos->itemClicked(repos->item(0));
        QCOMPARE(listed.constLast().value("repository_path").toString(), "/workspace/b");
        // A service that predates repository browsing lists nothing rather than the wrong one.
        QCOMPARE(list->count(), 0);
        QVERIFY(!use->isEnabled());
        QVERIFY(dialog->findChild<QLabel*>("worktreeStatus")->text().startsWith("Restart"));
        // After the service restarts, listing b again shows its worktrees.
        olderService = false;
        search->clear();
        emit repos->itemClicked(repos->item(0));
        QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), current);
        search->setText("b");
        emit repos->itemClicked(repos->item(0));
        QVERIFY(!acknowledgement);
        QCOMPARE(list->item(1)->data(Qt::UserRole).toString(), "/workspace/b-pilot");
        list->setCurrentItem(list->item(1));
        use->click();
        QVERIFY(acknowledgement);
        QCOMPARE(mutation.value("worktree_path").toString(), "/workspace/b-pilot");
        QCOMPARE(mutation.value("expected_context_revision").toInt(), 4);
        QVERIFY(!use->isEnabled());

        // A failure keeps the dialog and the current worktree; Use retries.
        acknowledgement({}, "Mac offline");
        QVERIFY(selected.isEmpty());
        QVERIFY(dialog->isVisible());
        QVERIFY(use->isEnabled());
        QCOMPARE(dialog->findChild<QLabel*>("worktreeStatus")->text(), "Mac offline");
        for (auto mode : {ketplus::ThemeManager::Mode::Light, ketplus::ThemeManager::Mode::Dark}) {
            theme.setMode(mode);
            QTest::qWait(30);
            QLabel* title = nullptr;
            for (auto* label : dialog->card()->findChildren<QLabel*>())
                if (label->text() == QStringLiteral("Repository & worktree"))
                    title = label;
            QVERIFY(title);
            QCOMPARE(title->palette().color(QPalette::WindowText), dialog->recipe().title);
            QCOMPARE(repos->parentWidget()->width(), 222);
            QVERIFY(list->width() >= 300);
            // A repository is one line and a worktree two, as the mock's rows.
            QVERIFY(repos->count() > 0 && list->count() > 0);
            const int repoRow = repos->visualItemRect(repos->item(0)).height();
            const int treeRow = list->visualItemRect(list->item(0)).height();
            QVERIFY2(repoRow >= 32 && repoRow <= 40, qPrintable(QString::number(repoRow)));
            QVERIFY2(treeRow > repoRow + 10, qPrintable(QString::number(treeRow)));
            if (const auto dir = qEnvironmentVariable("KETPLUS_PICKER_CAPTURE_DIR"); !dir.isEmpty())
                dialog->grab().save(dir + (mode == ketplus::ThemeManager::Mode::Light
                                               ? "/picker-light.png"
                                               : "/picker-dark.png"));
        }
        acknowledgement = {};
        use->click();
        QVERIFY(acknowledgement);
        const QPointer<ket::Dialog> open(dialog);
        acknowledgement({{"ok", true}, {"worktree_path", "/workspace/b-pilot"}}, {});
        QCOMPARE(selected, "/workspace/b-pilot");
        // The next turn's place is announced, and the picker closes.
        auto* toast = parent.findChild<ket::Toast*>();
        QVERIFY(toast);
        QCOMPARE(toast->text(), QStringLiteral("Next turn: b · feat/pilot"));
        QTRY_VERIFY(open.isNull() || !open->isVisible());
        QVERIFY(!created);
    }
    // #405 review: a failed listing, a pending search, the arrow keys and a stale context.
    void choiceFollowsTheListAndRecoversAStaleContext() {
        QWidget parent; // the picker is a card inside this window
        parent.resize(900, 600);
        parent.show();
        QList<QJsonObject> switches;
        QList<QJsonObject> listings;
        std::function<void(const QJsonObject&, const QString&)> pendingListing;
        bool offline = false;
        bool holdListing = false;
        qint64 revision = 4;
        bool stale = true;
        ketplus::showExecutionWorktreeDialog(
            &parent,
            [&](const QString& type, const QJsonObject& payload, auto done) {
                if (type == "session.worktree.switch") {
                    switches.append(payload);
                    if (stale) {
                        stale = false;
                        revision = 5;
                        done({{"ok", false},
                              {"error_code", "stale_context"},
                              {"error_message", "Refresh the conversation's execution context."}},
                             {});
                        return;
                    }
                    done({{"ok", true}, {"worktree_path", payload.value("worktree_path")}}, {});
                    return;
                }
                listings.append(payload);
                if (offline) {
                    done({}, "Offline");
                    return;
                }
                const auto repository = payload.value("repository_path").toString("/workspace/a");
                QJsonObject result{
                    {"ok", true},
                    {"repository_path", repository},
                    {"worktree_path", "/workspace/a"},
                    {"context_revision", revision},
                    {"switch_supported", true},
                    {"total", 3},
                    {"worktrees",
                     QJsonArray{
                         QJsonObject{{"branch", "main"}, {"worktree_path", repository}},
                         QJsonObject{{"branch", "one"}, {"worktree_path", repository + "-1"}},
                         QJsonObject{{"branch", "two"}, {"worktree_path", repository + "-2"}}}}};
                if (holdListing) {
                    pendingListing = [done, result](const QJsonObject&, const QString&) {
                        done(result, {});
                    };
                    return;
                }
                done(result, {});
            },
            [](const QString&) {}, "/workspace/a", {"/workspace/a", "/workspace/b"},
            "/workspace/a");
        auto* dialog = parent.findChild<ket::Dialog*>("executionWorktreeDialog");
        auto* repos = dialog->findChild<QListWidget*>("executionRepositoryList");
        auto* list = dialog->findChild<QListWidget*>("executionWorktreeList");
        auto* use = dialog->findChild<QAbstractButton*>("useRepositoryButton");
        QCOMPARE(list->count(), 3);

        // A failed listing of b leaves nothing of a to choose.
        offline = true;
        emit repos->itemClicked(repos->item(1));
        QCOMPARE(listings.constLast().value("repository_path").toString(), "/workspace/b");
        QCOMPARE(list->count(), 0);
        QVERIFY(!use->isEnabled());
        use->click();
        QVERIFY(switches.isEmpty());
        offline = false;

        // The arrow keys move the current repository and the chosen worktree.
        repos->setFocus();
        repos->setCurrentItem(repos->item(1));
        QTest::keyClick(repos, Qt::Key_Up);
        QCOMPARE(listings.constLast().contains("repository_path"), false);
        QCOMPARE(list->count(), 3);
        list->setFocus();
        list->setCurrentRow(0);
        QTest::keyClick(list, Qt::Key_Down);
        QTest::keyClick(list, Qt::Key_Down);
        QVERIFY(use->isEnabled());

        // Use waits while a changed search has not answered.
        holdListing = true;
        auto* search = dialog->findChild<QLineEdit*>("worktreeSearch");
        search->setText("t");
        QVERIFY(!use->isEnabled());
        QTRY_VERIFY(pendingListing);
        QVERIFY(!use->isEnabled());
        holdListing = false;
        std::exchange(pendingListing, {})({}, {});
        list->setCurrentRow(2);
        QVERIFY(use->isEnabled());

        // A failed search leaves no row of the previous query to choose.
        offline = true;
        search->setText("tw");
        QTRY_COMPARE(list->count(), 0);
        QVERIFY(!use->isEnabled());
        offline = false;
        search->setText("t");
        QTRY_COMPARE(list->count(), 3);
        list->setCurrentRow(2);
        QVERIFY(use->isEnabled());

        // A stale context reads the conversation again and keeps the choice; Use then sends
        // the new revision.
        use->click();
        QCOMPARE(switches.size(), 1);
        QCOMPARE(switches.constLast().value("worktree_path").toString(), "/workspace/a-2");
        QCOMPARE(switches.constLast().value("expected_context_revision").toInt(), 4);
        QTRY_COMPARE(list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString()
                                         : QString(),
                     QStringLiteral("/workspace/a-2"));
        QVERIFY(dialog->findChild<QLabel*>("worktreeStatus")
                    ->text()
                    .startsWith("The conversation changed elsewhere"));
        use->click();
        QCOMPARE(switches.size(), 2);
        QCOMPARE(switches.constLast().value("expected_context_revision").toInt(), 5);
        QCOMPARE(switches.constLast().value("worktree_path").toString(), "/workspace/a-2");
    }
    void createWorktreeOpensTheWorktreesPage() {
        QWidget parent; // the picker is a card inside this window
        parent.resize(900, 600);
        parent.show();
        bool created = false;
        ketplus::showExecutionWorktreeDialog(
            &parent, [](const QString&, const QJsonObject&, auto) {}, [](const QString&) {}, {}, {},
            {}, [&] { created = true; });
        auto* dialog = parent.findChild<ket::Dialog*>("executionWorktreeDialog");
        auto* create = dialog->findChild<QAbstractButton*>("createWorktreeButton");
        QVERIFY(create && create->isVisibleTo(dialog));
        create->click();
        QVERIFY(created);
    }
};
QTEST_MAIN(ExecutionWorktreeDialogTest)
#include "ExecutionWorktreeDialogTest.moc"
