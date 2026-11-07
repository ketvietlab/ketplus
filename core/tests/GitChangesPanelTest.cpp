#include "git/GitChangesPanel.h"
#include "ui/Theme.h"

#include "KetAppearance.h"
#include "KetControlRecipes.h"
#include "KetSurfaceRecipes.h"
#include <QLabel>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTreeWidget>
#include <QtTest>

class GitChangesPanelTest final : public QObject {
    Q_OBJECT

  private slots:
    void rendersRepositoryAndGroupedFiles();
    void keepsSelectionAcrossRefreshes();
};

void GitChangesPanelTest::rendersRepositoryAndGroupedFiles() {
    ketplus::ThemeManager theme(*qApp);
    theme.setMode(ketplus::ThemeManager::Mode::Light);
    ketplus::GitSnapshot snapshot;
    snapshot.repositoryRoot = QStringLiteral("/tmp/ketplus-desktop");
    snapshot.branch = QStringLiteral("develop");
    snapshot.files = {
        {QStringLiteral("modules/ai/src/agent/qml/NavigationRail.qml"),
         {},
         QLatin1Char('.'),
         QLatin1Char('M')},
        {QStringLiteral("modules/ai/src/agent/qml/WorktreeItem.qml"),
         {},
         QLatin1Char('.'),
         QLatin1Char('M')},
        {QStringLiteral("apps/desktop/src/ui/ThemeManager.cpp"),
         {},
         QLatin1Char('M'),
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
    QCOMPARE(tree->itemDelegate()->sizeHint({}, tree->model()->index(0, 0)).height(),
             qRound(ket::groupHeaderRecipe(false, {}, ket::Theme::Light).height));
    QCOMPARE(tree->itemDelegate()
                 ->sizeHint({}, tree->model()->index(0, 0, tree->model()->index(0, 0)))
                 .height(),
             qRound(ket::fileRowRecipe(false, {}, ket::Theme::Light).height));

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

void GitChangesPanelTest::keepsSelectionAcrossRefreshes() {
    ketplus::ThemeManager theme(*qApp);
    ketplus::GitSnapshot snapshot;
    snapshot.repositoryRoot = QStringLiteral("/tmp/ketplus-refresh");
    snapshot.branch = QStringLiteral("develop");
    for (int index = 0; index < 40; ++index)
        snapshot.files.append({QStringLiteral("src/file%1.cpp").arg(index, 2, 10, QLatin1Char('0')),
                               {},
                               QLatin1Char('.'),
                               QLatin1Char('M')});

    ketplus::GitChangesPanel panel;
    panel.resize(224, 320);
    panel.setSnapshot(snapshot);
    panel.show();
    QTest::qWait(50);
    auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("sourceControlFiles"));
    QVERIFY(tree);
    auto* target = tree->topLevelItem(0)->child(30);
    tree->setCurrentItem(target);
    tree->scrollToItem(target);
    const int scroll = tree->verticalScrollBar()->value();
    QVERIFY(scroll > 0);

    // A save or a Git command refreshes with an unrelated new file at the top.
    snapshot.files.prepend({QStringLiteral("aaa/new.txt"), {}, QLatin1Char('.'), QLatin1Char('M')});
    QSignalSpy diffRequested(&panel, &ketplus::GitChangesPanel::diffRequested);
    panel.setSnapshot(snapshot);
    QVERIFY(tree->currentItem() != nullptr);
    QCOMPARE(tree->currentItem()->text(0), QStringLiteral("file30.cpp"));
    QCOMPARE(tree->verticalScrollBar()->value(), scroll);
    QCOMPARE(diffRequested.count(), 0);
}

QTEST_MAIN(GitChangesPanelTest)
#include "GitChangesPanelTest.moc"
