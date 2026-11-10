#include "app/ExecutionWorktreeDialog.h"
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

class ExecutionWorktreeDialogTest final : public QObject {
    Q_OBJECT
  private slots:
    void scrollSearchAndSelectionSwitchExecutionAfterAcknowledgement() {
        QWidget parent;
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
        auto* dialog = parent.findChild<QDialog*>();
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
        emit list->itemClicked(list->item(1));
        QVERIFY(switched.isEmpty());
        QVERIFY(use->isEnabled());
        const QPointer<QDialog> open(dialog);
        use->click();
        QCOMPARE(switched.value("expected_context_revision").toInt(), 7);
        QCOMPARE(switched.value("worktree_path").toString(), "/repo-1");
        QCOMPARE(selected, "/repo-1");
        QTRY_VERIFY(open.isNull() || !open->isVisible());
    }
    void repositoryListsItsWorktreesThenUseSwitchesAfterAck() {
        QWidget parent;
        parent.resize(900, 600);
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
        auto* dialog = parent.findChild<QDialog*>("executionWorktreeDialog");
        QVERIFY(dialog);
        QCOMPARE(dialog->windowTitle(), QStringLiteral("Repository & worktree"));
        auto* repos = dialog->findChild<QListWidget*>("executionRepositoryList");
        auto* list = dialog->findChild<QListWidget*>("executionWorktreeList");
        auto* use = dialog->findChild<QAbstractButton*>("useRepositoryButton");
        QVERIFY(repos && list && use);
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
        emit list->itemClicked(list->item(1));
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
        ketplus::ThemeManager theme(*qApp);
        for (auto mode : {ketplus::ThemeManager::Mode::Light, ketplus::ThemeManager::Mode::Dark}) {
            theme.setMode(mode);
            QTest::qWait(30);
            QCOMPARE(repos->parentWidget()->width(), 222);
            QVERIFY(list->width() >= 300);
            if (const auto dir = qEnvironmentVariable("KETPLUS_PICKER_CAPTURE_DIR"); !dir.isEmpty())
                dialog->grab().save(dir + (mode == ketplus::ThemeManager::Mode::Light
                                               ? "/picker-light.png"
                                               : "/picker-dark.png"));
        }
        acknowledgement = {};
        use->click();
        QVERIFY(acknowledgement);
        const QPointer<QDialog> open(dialog);
        acknowledgement({{"ok", true}, {"worktree_path", "/workspace/b-pilot"}}, {});
        QCOMPARE(selected, "/workspace/b-pilot");
        // The next turn's place is announced, and the picker closes.
        auto* toast = parent.findChild<ket::Toast*>();
        QVERIFY(toast);
        QCOMPARE(toast->text(), QStringLiteral("Next turn: b · feat/pilot"));
        QTRY_VERIFY(open.isNull() || !open->isVisible());
        QVERIFY(!created);
    }
    void createWorktreeOpensTheWorktreesPage() {
        QWidget parent;
        bool created = false;
        ketplus::showExecutionWorktreeDialog(
            &parent, [](const QString&, const QJsonObject&, auto) {}, [](const QString&) {}, {}, {},
            {}, [&] { created = true; });
        auto* dialog = parent.findChild<QDialog*>("executionWorktreeDialog");
        auto* create = dialog->findChild<QAbstractButton*>("createWorktreeButton");
        QVERIFY(create && create->isVisibleTo(dialog));
        create->click();
        QVERIFY(created);
    }
};
QTEST_MAIN(ExecutionWorktreeDialogTest)
#include "ExecutionWorktreeDialogTest.moc"
