#include "workspace/ExplorerPanel.h"

#include <QDir>
#include <QFile>
#include <QFileSystemModel>
#include <QLineEdit>
#include <QMetaObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>

class ExplorerPanelTest final : public QObject {
    Q_OBJECT

  private slots:
    void loadsOnlyAfterReceivingARoot();
    void opensFilesFromTheTree();
    void togglesFoldersFromTheTree();
    void revealsAnOpenFile();
    void reportsInlineRenames();
    void createsInlineWithoutOverwritingExistingFiles();
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
