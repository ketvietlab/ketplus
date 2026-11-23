#include "workspace/ExplorerPanel.h"
#include "KetAppearance.h"
#include "KetSurfaceRecipes.h"
#include "workspace/TreeRowColumns.h"

#include <QDir>
#include <QFile>
#include <QFileSystemModel>
#include <QImage>
#include <QLineEdit>
#include <QMetaObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>

#include <utility>

class ExplorerPanelTest final : public QObject {
    Q_OBJECT

  private slots:
    void loadsOnlyAfterReceivingARoot();
    void updatesTheLinkedWorktreeAfterExternalFileChanges();
    void opensFilesFromTheTree();
    void togglesFoldersFromTheTree();
    void revealsAnOpenFile();
    void reportsInlineRenames();
    void createsInlineWithoutOverwritingExistingFiles();
    void filesAndFoldersShareTheIconAndNameColumns();
};

void ExplorerPanelTest::createsInlineWithoutOverwritingExistingFiles() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ketplus::ExplorerPanel panel;
    panel.resize(360, 600);
    panel.setRootPath(directory.path());
    panel.show();
    panel.createEntry(false);
    auto* field = panel.findChild<QLineEdit*>(QStringLiteral("explorerNewEntryName"));
    QVERIFY(field);
    field->setText(QStringLiteral("../outside.txt"));
    QTest::keyClick(field, Qt::Key_Return);
    QVERIFY(field->isVisible());
    QSignalSpy opened(&panel, &ketplus::ExplorerPanel::fileOpenRequested);
    field->setText(QStringLiteral("notes.md"));
    QTest::keyClick(field, Qt::Key_Return);
    QCOMPARE(opened.count(), 1);
    QVERIFY(QFile::exists(directory.filePath("notes.md")));
    QVERIFY(!field->isVisible());
    QFile file(directory.filePath("notes.md"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("keep me");
    file.close();
    panel.createEntry(false);
    field->setText(QStringLiteral("notes.md"));
    QTest::keyClick(field, Qt::Key_Return);
    QVERIFY(field->isVisible());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("keep me"));
    QTest::keyClick(field, Qt::Key_Escape);
    QTRY_VERIFY(!field->isVisible());
}

namespace {

// Whether any pixel of `image` in columns [from, to) of the `row` differs from `background`.
// The outer two pixels of the row are left out: the keyboard focus ring runs along them.
bool hasInk(const QImage& image, const QRect& row, const int from, const int to,
            const QColor& background) {
    for (int y = row.top() + 2; y <= row.bottom() - 2 && y < image.height(); ++y)
        for (int x = qMax(2, from); x < to && x < image.width(); ++x)
            if (image.pixelColor(x, y) != background)
                return true;
    return false;
}

} // namespace

// A file keeps the folder's chevron slot free, so its icon and its name start where a folder's
// do, at every depth and interface zoom.
void ExplorerPanelTest::filesAndFoldersShareTheIconAndNameColumns() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("docs/guides")));
    for (const QString& name : {QStringLiteral("a.txt"), QStringLiteral("docs/b.txt")}) {
        QFile file(directory.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    ketplus::ExplorerPanel panel;
    panel.resize(420, 400);
    panel.setRootPath(directory.path());
    panel.show();
    auto* tree = panel.findChild<QTreeView*>(QStringLiteral("explorerTree"));
    QVERIFY(tree != nullptr);
    auto* model = qobject_cast<QFileSystemModel*>(tree->model());
    QVERIFY(model != nullptr);
    tree->expand(model->index(directory.filePath(QStringLiteral("docs"))));
    // The row showing `name`, found where the tree draws it.
    const auto rowOf = [tree](const QString& name) {
        for (int y = 1; y < tree->viewport()->height(); y += 4) {
            const QModelIndex index = tree->indexAt(QPoint(60, y));
            if (index.isValid() && index.data().toString() == name)
                return std::pair{index, tree->visualRect(index)};
        }
        return std::pair{QModelIndex(), QRect()};
    };
    QTRY_VERIFY_WITH_TIMEOUT(rowOf(QStringLiteral("b.txt")).first.isValid(), 3000);

    auto& appearance = ket::Appearance::instance();
    const qreal zoom = appearance.zoom();
    for (const qreal step : {1.0, 1.25, 1.5}) {
        appearance.setZoom(step);
        QTest::qWait(50);
        const QImage image = tree->viewport()->grab().toImage();
        const auto px = [&appearance](qreal v) { return qRound(appearance.scaled(v)); };
        const auto recipe = ket::fileRowRecipe(false, {}, appearance.theme());
        // Entries of the open folder are depth 1; the ones inside `docs` are depth 2.
        const struct {
            QString name;
            bool isFolder;
            int depth;
        } rows[] = {{QStringLiteral("docs"), true, 1},
                    {QStringLiteral("a.txt"), false, 1},
                    {QStringLiteral("b.txt"), false, 2}};
        for (const auto& row : rows) {
            QTRY_VERIFY_WITH_TIMEOUT(!rowOf(row.name).second.isEmpty(), 3000);
            const QRect rect = rowOf(row.name).second;
            const QColor background = image.pixelColor(image.width() - 8, rect.center().y());
            const auto columns = ketplus::tree_rows::explorerColumns(recipe, row.depth, px);
            const QString label = QStringLiteral("zoom %1, depth %2, %3")
                                      .arg(step)
                                      .arg(row.depth)
                                      .arg(row.isFolder ? "folder" : "file");
            // Nothing but a folder's chevron is left of the icon column.
            QVERIFY2(hasInk(image, rect, columns.chevron, columns.icon, background) == row.isFolder,
                     qPrintable(label));
            QVERIFY2(!hasInk(image, rect, 0, columns.chevron, background), qPrintable(label));
            // The icon is in the shared column, and nothing is drawn between it and the name.
            QVERIFY2(
                hasInk(image, rect, columns.icon, columns.icon + px(recipe.iconSize), background),
                qPrintable(label));
            QVERIFY2(
                !hasInk(image, rect, columns.icon + px(recipe.iconSize), columns.text, background),
                qPrintable(label));
        }
    }
    appearance.setZoom(zoom);
}

void ExplorerPanelTest::reportsInlineRenames() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("notes.md"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    ketplus::ExplorerPanel explorer;
    explorer.show();
    explorer.setRootPath(directory.path());
    auto* tree = explorer.findChild<QTreeView*>(QStringLiteral("explorerTree"));
    QVERIFY(tree != nullptr);
    auto* model = qobject_cast<QFileSystemModel*>(tree->model());
    QVERIFY(model != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(model->index(path).isValid(), 3000);

    QSignalSpy renamed(&explorer, &ketplus::ExplorerPanel::pathRenamed);
    // What the tree's inline editor commits.
    QVERIFY(model->setData(model->index(path), QStringLiteral("todo.md")));
    QCOMPARE(renamed.count(), 1);
    QCOMPARE(QDir::cleanPath(renamed.constFirst().at(0).toString()), QDir::cleanPath(path));
    QCOMPARE(QDir::cleanPath(renamed.constFirst().at(1).toString()),
             QDir::cleanPath(directory.filePath(QStringLiteral("todo.md"))));
}

void ExplorerPanelTest::revealsAnOpenFile() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir root(directory.path());
    QVERIFY(root.mkpath(QStringLiteral("src/app")));
    const QString path = root.filePath(QStringLiteral("src/app/main.cpp"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    ketplus::ExplorerPanel explorer;
    explorer.resize(280, 500);
    explorer.show();
    explorer.setRootPath(directory.path());

    auto* tree = explorer.findChild<QTreeView*>(QStringLiteral("explorerTree"));
    QVERIFY(tree != nullptr);
    auto* model = qobject_cast<QFileSystemModel*>(tree->model());
    QVERIFY(model != nullptr);

    // The folders on the way are not loaded yet, so revealing finishes as they arrive.
    explorer.revealPath(path);
    QTRY_VERIFY_WITH_TIMEOUT(model->filePath(tree->currentIndex()) == path, 3000);
    QVERIFY(tree->isExpanded(model->index(root.filePath(QStringLiteral("src")))));
    QVERIFY(tree->isExpanded(model->index(root.filePath(QStringLiteral("src/app")))));

    // A file outside the open folder leaves the selection alone.
    QTemporaryDir elsewhere;
    QVERIFY(elsewhere.isValid());
    explorer.revealPath(QDir(elsewhere.path()).filePath(QStringLiteral("other.cpp")));
    QCOMPARE(model->filePath(tree->currentIndex()), path);
}

void ExplorerPanelTest::loadsOnlyAfterReceivingARoot() {
    ketplus::ExplorerPanel explorer;
    QCOMPARE(explorer.rootPath(), QString());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("nested")));
    QFile file(directory.filePath(QStringLiteral("readme.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("KetPlus"), 7);
    file.close();

    explorer.setRootPath(directory.path());
    QCOMPARE(explorer.rootPath(), directory.path());

    auto* tree = explorer.findChild<QTreeView*>(QStringLiteral("explorerTree"));
    QVERIFY(tree != nullptr);
    auto* model = qobject_cast<QFileSystemModel*>(tree->model());
    QVERIFY(model != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(model->rowCount(tree->rootIndex()) >= 2, 3000);
    QVERIFY(model->index(file.fileName()).isValid());
}

void ExplorerPanelTest::updatesTheLinkedWorktreeAfterExternalFileChanges() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("repo")));
    const QString repository = directory.filePath(QStringLiteral("repo"));
    const QString worktree = directory.filePath(QStringLiteral("worktree"));
    const auto git = [&repository](const QStringList& arguments) {
        QProcess process;
        process.setWorkingDirectory(repository);
        process.start(QStringLiteral("git"), arguments);
        QVERIFY(process.waitForFinished());
        QCOMPARE(process.exitCode(), 0);
    };
    git({"init", "-q", "-b", "main"});
    QFile tracked(QDir(repository).filePath(QStringLiteral("tracked.txt")));
    QVERIFY(tracked.open(QIODevice::WriteOnly));
    tracked.close();
    git({"add", "tracked.txt"});
    git({"-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "-c",
         "commit.gpgsign=false", "commit", "-qm", "Initial commit"});
    git({"worktree", "add", "-qb", "linked", worktree});

    const QDir checkout(worktree);
    QVERIFY(checkout.mkdir(QStringLiteral("large")));
    for (int index = 0; index < 2'000; ++index) {
        QFile file(checkout.filePath(QStringLiteral("large/entry-%1.txt").arg(index)));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    ketplus::ExplorerPanel explorer;
    explorer.setRootPath(worktree);
    auto* tree = explorer.findChild<QTreeView*>(QStringLiteral("explorerTree"));
    QVERIFY(tree);
    auto* model = tree->model();
    const auto names = [&] {
        QStringList result;
        const QModelIndex root = tree->rootIndex();
        for (int row = 0; row < model->rowCount(root); ++row)
            result.append(model->index(row, 0, root).data().toString());
        return result;
    };
    const QStringList rootEntries{QStringLiteral("large"), QStringLiteral("tracked.txt")};
    QTRY_COMPARE(names(), rootEntries);
    const QModelIndex folder = model->index(0, 0, tree->rootIndex());
    QVERIFY(model->canFetchMore(folder));
    QCOMPARE(model->rowCount(folder), 0);
    const QString unicodeName = QStringLiteral("tệp-雪.txt");
    QFile external(QDir(worktree).filePath(unicodeName));
    QVERIFY(external.open(QIODevice::WriteOnly));
    external.close();
    QTRY_VERIFY(names().contains(unicodeName));
    QVERIFY(external.remove());
    QTRY_COMPARE(names(), rootEntries);
}

void ExplorerPanelTest::opensFilesFromTheTree() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("main.cpp"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    ketplus::ExplorerPanel explorer;
    explorer.resize(280, 500);
    explorer.show();
    explorer.setRootPath(directory.path());

    auto* tree = explorer.findChild<QTreeView*>(QStringLiteral("explorerTree"));
    QVERIFY(tree != nullptr);
    auto* model = qobject_cast<QFileSystemModel*>(tree->model());
    QVERIFY(model != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(model->index(path).isValid(), 3000);

    const QModelIndex index = model->index(path);
    QSignalSpy opened(&explorer, &ketplus::ExplorerPanel::fileOpenRequested);
    QVERIFY(QMetaObject::invokeMethod(tree, "clicked", Qt::DirectConnection,
                                      Q_ARG(QModelIndex, index)));

    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.constFirst().constFirst().toString(), path);
}

void ExplorerPanelTest::togglesFoldersFromTheTree() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("nested")));
    const QString path = directory.filePath(QStringLiteral("nested"));

    ketplus::ExplorerPanel explorer;
    explorer.setRootPath(directory.path());

    auto* tree = explorer.findChild<QTreeView*>(QStringLiteral("explorerTree"));
    QVERIFY(tree != nullptr);
    auto* model = qobject_cast<QFileSystemModel*>(tree->model());
    QVERIFY(model != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(model->index(path).isValid(), 3000);

    const QModelIndex index = model->index(path);
    QSignalSpy opened(&explorer, &ketplus::ExplorerPanel::fileOpenRequested);
    QVERIFY(!tree->isExpanded(index));

    QVERIFY(QMetaObject::invokeMethod(tree, "clicked", Qt::DirectConnection,
                                      Q_ARG(QModelIndex, index)));
    QVERIFY(tree->isExpanded(index));
    QCOMPARE(opened.count(), 0);

    QVERIFY(QMetaObject::invokeMethod(tree, "clicked", Qt::DirectConnection,
                                      Q_ARG(QModelIndex, index)));
    QVERIFY(!tree->isExpanded(index));
    QCOMPARE(opened.count(), 0);
}

QTEST_MAIN(ExplorerPanelTest)
#include "ExplorerPanelTest.moc"
