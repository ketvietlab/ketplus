#include "git/GitChangesPanel.h"
#include "ui/Theme.h"
#include "workspace/TreeRowColumns.h"

#include "KetAppearance.h"
#include "KetControlRecipes.h"
#include "KetSurfaceRecipes.h"
#include <QImage>
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
    void aFilesIconSitsUnderItsGroupsTitle();
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

// The icon of a changed file starts where its group's title does, the chevron's width in,
// at every interface zoom.
void GitChangesPanelTest::aFilesIconSitsUnderItsGroupsTitle() {
    ketplus::ThemeManager theme(*qApp);
    ketplus::GitSnapshot snapshot;
    snapshot.repositoryRoot = QStringLiteral("/tmp/ketplus-columns");
    snapshot.branch = QStringLiteral("develop");
    snapshot.files = {{QStringLiteral("src/main.cpp"), {}, QLatin1Char('.'), QLatin1Char('M')}};
    ketplus::GitChangesPanel panel;
    panel.resize(320, 900);
    panel.setSnapshot(snapshot);
    panel.show();
    QTest::qWait(50);
    auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("sourceControlFiles"));
    QVERIFY(tree);
    auto* group = tree->topLevelItem(0);
    QVERIFY(group && group->childCount() == 1);
    group->setExpanded(true);

    const auto hasInk = [](const QImage& image, const QRect& row, int from, int to,
                           const QColor& background) {
        // The outer two pixels of the row are left out: the keyboard focus ring runs along them.
        for (int y = row.top() + 2; y <= row.bottom() - 2 && y < image.height(); ++y)
            for (int x = qMax(2, from); x < to && x < image.width(); ++x)
                if (image.pixelColor(x, y) != background)
                    return true;
        return false;
    };
    auto& appearance = ket::Appearance::instance();
    const qreal zoom = appearance.zoom();
    for (const qreal step : {1.0, 1.25, 1.5}) {
        appearance.setZoom(step);
        QTest::qWait(50);
        const QImage image = tree->viewport()->grab().toImage();
        const auto px = [&appearance](qreal v) { return qRound(appearance.scaled(v)); };
        const auto file = ket::fileRowRecipe(false, {}, appearance.theme());
        const auto header = ket::groupHeaderRecipe(false, {}, appearance.theme());
        const auto columns = ketplus::tree_rows::sourceControlColumns(file, header, px);
        const QRect groupRow = tree->visualItemRect(group);
        const QRect fileRow = tree->visualItemRect(group->child(0));
        const QColor groupBackground = image.pixelColor(image.width() - 8, groupRow.center().y());
        const QColor fileBackground = image.pixelColor(image.width() - 8, fileRow.center().y());
        const QString label = QStringLiteral("zoom %1").arg(step);
        // The group: its chevron, then its title.
        QVERIFY2(hasInk(image, groupRow, columns.groupChevron, columns.groupText, groupBackground),
                 qPrintable(label));
        // The file: nothing before the group's title column, then the icon.
        QVERIFY2(!hasInk(image, fileRow, 0, columns.fileIcon, fileBackground), qPrintable(label));
        QVERIFY2(hasInk(image, fileRow, columns.fileIcon, columns.fileIcon + px(file.iconSize),
                        fileBackground),
                 qPrintable(label));
        QVERIFY2(!hasInk(image, fileRow, columns.fileIcon + px(file.iconSize), columns.fileText,
                         fileBackground),
                 qPrintable(label));
        QCOMPARE(columns.fileIcon, columns.groupText);
    }
    appearance.setZoom(zoom);
}

QTEST_MAIN(GitChangesPanelTest)
#include "GitChangesPanelTest.moc"
