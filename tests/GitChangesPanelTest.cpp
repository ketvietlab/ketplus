#include "git/GitChangesPanel.h"
#include "ui/Theme.h"

#include <QLabel>
#include <QSignalSpy>
#include <QTreeWidget>
#include <QtTest>

class GitChangesPanelTest final : public QObject {
    Q_OBJECT

  private slots:
    void rendersRepositoryAndGroupedFiles();
};

void GitChangesPanelTest::rendersRepositoryAndGroupedFiles() {
    ketplus::ThemeManager theme(*qApp);
    theme.setMode(ketplus::ThemeManager::Mode::Light);
    ketplus::GitSnapshot snapshot;
    snapshot.repositoryRoot = QStringLiteral("/tmp/ketplus-desktop");
    snapshot.branch = QStringLiteral("develop");
    snapshot.files = {
        {QStringLiteral("modules/ai/src/agent/qml/NavigationRail.qml"), {}, QLatin1Char('.'),
         QLatin1Char('M')},
        {QStringLiteral("modules/ai/src/agent/qml/WorktreeItem.qml"), {}, QLatin1Char('.'),
         QLatin1Char('M')},
        {QStringLiteral("apps/desktop/src/ui/ThemeManager.cpp"), {}, QLatin1Char('M'),
         QLatin1Char('.')},
    };

    ketplus::GitChangesPanel panel;
    panel.resize(224, 640);
    panel.setSnapshot(snapshot);
    panel.show();
    QTest::qWait(50);

    auto* repository = panel.findChild<QLabel*>(QStringLiteral("sourceControlRepositoryName"));
    auto* branch = panel.findChild<QLabel*>(QStringLiteral("sourceControlBranch"));
    auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("sourceControlFiles"));
    QVERIFY(repository);
    QVERIFY(branch);
    QVERIFY(tree);
    QCOMPARE(repository->text(), QStringLiteral("ketplus-desktop"));
    QCOMPARE(branch->text(), QStringLiteral("develop"));
    QCOMPARE(tree->topLevelItemCount(), 2);

    auto* changes = tree->topLevelItem(0);
    auto* staged = tree->topLevelItem(1);
    QCOMPARE(changes->text(0), QStringLiteral("Changes"));
    QCOMPARE(changes->data(0, Qt::UserRole + 3).toInt(), 2);
    QCOMPARE(changes->childCount(), 2);
    QCOMPARE(staged->text(0), QStringLiteral("Staged Changes"));
    QCOMPARE(staged->data(0, Qt::UserRole + 3).toInt(), 1);
    QCOMPARE(changes->child(0)->text(0), QStringLiteral("NavigationRail.qml"));
    QCOMPARE(changes->child(0)->data(0, Qt::UserRole + 2).toString(),
             QStringLiteral("modules/ai/src/agent/qml"));
    QCOMPARE(changes->child(0)->text(1), QStringLiteral("M"));
    QCOMPARE(staged->child(0)->text(1), QStringLiteral("M"));
    QCOMPARE(tree->columnWidth(1), 32);
    QCOMPARE(tree->itemDelegate()->sizeHint({}, tree->model()->index(0, 0)).height(), 40);
    QCOMPARE(tree->itemDelegate()->sizeHint({}, tree->model()->index(0, 0, tree->model()->index(0, 0)))
                 .height(),
             52);

    panel.focusChanges();
    QCOMPARE(tree->currentItem(), changes->child(0));
    const auto capture = qEnvironmentVariable("KETPLUS_GIT_PANEL_CAPTURE");
    if (!capture.isEmpty())
        QVERIFY(panel.grab().save(capture));
    QSignalSpy diffRequested(&panel, &ketplus::GitChangesPanel::diffRequested);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      tree->visualItemRect(changes->child(0)).center());
    QCOMPARE(diffRequested.count(), 1);
    QCOMPARE(diffRequested.constFirst().at(1).value<ketplus::GitDiffMode>(),
             ketplus::GitDiffMode::Unstaged);
}

QTEST_MAIN(GitChangesPanelTest)
#include "GitChangesPanelTest.moc"
