#include "app/ExecutionWorktreeDialog.h"
#include "ui/Theme.h"
#include <QJsonArray>
#include <QLineEdit>
#include <QListWidget>
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
        dialog->grab().save("/tmp/ketplus-worktree-desktop-ui.png");
        emit list->itemClicked(list->item(0));
        QCOMPARE(switched.value("expected_context_revision").toInt(), 7);
        QCOMPARE(switched.value("worktree_path").toString(), "/repo-0");
        QVERIFY(dialog->isVisible());
        QCOMPARE(selected, "/repo-0");
    }
    void repositorySwitchWaitsForAckAndFailureKeepsCurrent() {
        QWidget parent;
        QString selected;
        QJsonObject mutation;
        std::function<void(const QJsonObject&, const QString&)> acknowledgement;
        QString current = "/workspace/a";
        qint64 revision = 4;
        ketplus::showExecutionWorktreeDialog(
            &parent,
            [&](const QString& type, const QJsonObject& payload, auto done) {
                if (type == "session.worktree.switch") {
                    mutation = payload;
                    acknowledgement = done;
                    return;
                }
                done({{"ok", true},
                      {"worktree_path", current},
                      {"context_revision", revision},
                      {"switch_supported", true},
                      {"switch_available", true},
                      {"total", 0},
                      {"worktrees", QJsonArray{}}},
                     {});
            },
            [&](const QString& path) { selected = path; }, current,
            {"/workspace/b", "/workspace/a"}, "/workspace/a");
        auto* dialog = parent.findChild<QDialog*>();
        auto* repos = dialog->findChild<QListWidget*>("executionRepositoryList");
        auto* search = dialog->findChild<QLineEdit*>("repositorySearch");
        QCOMPARE(repos->item(0)->data(Qt::UserRole).toString(), current);
        search->setText("b");
        QCOMPARE(repos->count(), 1);
        emit repos->itemClicked(repos->item(0));
        QVERIFY(acknowledgement);
        QCOMPARE(mutation.value("expected_context_revision").toInt(), 4);
        QVERIFY(selected.isEmpty());
        QVERIFY(!repos->isEnabled());
        acknowledgement({}, "Mac offline");
        QVERIFY(selected.isEmpty());
        QVERIFY(repos->isEnabled());
        search->clear();
        QCOMPARE(repos->item(0)->data(Qt::UserRole).toString(), current);
        emit repos->itemClicked(repos->item(1));
        current = "/workspace/b";
        revision = 5;
        acknowledgement({{"ok", true}, {"worktree_path", current}, {"context_revision", revision}},
                        {});
        QCOMPARE(selected, current);
        QCOMPARE(repos->item(0)->data(Qt::UserRole).toString(), current);
        QVERIFY(dialog->isVisible());
        ketplus::ThemeManager theme(*qApp);
        for (auto mode : {ketplus::ThemeManager::Mode::Light, ketplus::ThemeManager::Mode::Dark}) {
            theme.setMode(mode);
            QTest::qWait(30);
            QVERIFY(repos->width() >= 200);
            QVERIFY(dialog->findChild<QListWidget*>("executionWorktreeList")->width() >= 300);
            dialog->grab().save(mode == ketplus::ThemeManager::Mode::Light
                                    ? "/tmp/ketplus-repository-picker-desktop-light.png"
                                    : "/tmp/ketplus-repository-picker-desktop-dark.png");
        }
    }
};
QTEST_MAIN(ExecutionWorktreeDialogTest)
#include "ExecutionWorktreeDialogTest.moc"
