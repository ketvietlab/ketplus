#include "index/SymbolIndex.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {

bool writeFile(const QString& path, const QByteArray& content) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(content) == content.size();
}

ketplus::WorkspaceFileList listOf(const QStringList& paths) { return {paths, false}; }

} // namespace

class SymbolIndexTest final : public QObject {
    Q_OBJECT

  private slots:
    void findsDeclarationsWithoutScanning();
    void sharesFilesWithIdenticalContents();
    void dropsTheLeastRecentlyUsedWorkspaceOverBudget();
    void dropsIdleWorkspaces();
};

void SymbolIndexTest::findsDeclarationsWithoutScanning() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir root(directory.path());
    QVERIFY(writeFile(root.filePath(QStringLiteral("a.cpp")), "class Widget {};\nint main() {}\n"));
    QVERIFY(writeFile(root.filePath(QStringLiteral("logo.png")), "class Widget {};\n"));

    ketplus::SymbolIndex index;
    index.indexWorkspace(root.absolutePath(),
                         listOf({QStringLiteral("a.cpp"), QStringLiteral("logo.png")}));
    QVERIFY(index.hasWorkspace(root.absolutePath()));

    const auto hits = index.lookup(root.absolutePath(), QStringLiteral("Widget"));
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().relativePath, QStringLiteral("a.cpp"));
    QCOMPARE(hits.first().line, 1);
    // Assets are recognised by name, so the declaration inside the image is never read.
    QCOMPARE(index.fileCount(root.absolutePath()), 1);
    QVERIFY(index.lookup(root.absolutePath(), QStringLiteral("Missing")).isEmpty());
    QVERIFY(index.lookup(QStringLiteral("/nowhere"), QStringLiteral("Widget")).isEmpty());
}

void SymbolIndexTest::sharesFilesWithIdenticalContents() {
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid() && second.isValid());
    const QByteArray shared = "class Shared {};\n";
    QVERIFY(writeFile(QDir(first.path()).filePath(QStringLiteral("a.cpp")), shared));
    QVERIFY(writeFile(QDir(second.path()).filePath(QStringLiteral("a.cpp")), shared));

    ketplus::SymbolIndex index;
    index.indexWorkspace(first.path(), listOf({QStringLiteral("a.cpp")}));
    const qint64 afterFirst = index.memoryBytes();
    QVERIFY(afterFirst > 0);
    QCOMPARE(index.blobCount(), 1);

    // The second worktree holds the same contents, so nothing new is stored.
    index.indexWorkspace(second.path(), listOf({QStringLiteral("a.cpp")}));
    QCOMPARE(index.blobCount(), 1);
    QCOMPARE(index.memoryBytes(), afterFirst);
    QCOMPARE(index.lookup(second.path(), QStringLiteral("Shared")).size(), 1);

    // Dropping one workspace keeps the symbols the other one still uses.
    index.forgetWorkspace(first.path());
    QCOMPARE(index.blobCount(), 1);
    QCOMPARE(index.lookup(second.path(), QStringLiteral("Shared")).size(), 1);

    index.forgetWorkspace(second.path());
    QCOMPARE(index.blobCount(), 0);
    QCOMPARE(index.memoryBytes(), 0);
}

void SymbolIndexTest::dropsTheLeastRecentlyUsedWorkspaceOverBudget() {
    QTemporaryDir first;
    QTemporaryDir second;
    QTemporaryDir third;
    QVERIFY(first.isValid() && second.isValid() && third.isValid());
    int counter = 0;
    const auto fill = [&counter](const QString& path) {
        QByteArray content;
        for (int line = 0; line < 40; ++line) {
            content += QByteArray("class Type") + QByteArray::number(++counter) + " {};\n";
        }
        return writeFile(QDir(path).filePath(QStringLiteral("a.cpp")), content);
    };
    QVERIFY(fill(first.path()) && fill(second.path()) && fill(third.path()));

    // A budget of one workspace's worth, so adding a third drops the least recently used.
    ketplus::SymbolIndex sizing;
    sizing.indexWorkspace(first.path(), listOf({QStringLiteral("a.cpp")}));
    const qint64 oneWorkspace = sizing.memoryBytes();

    // Room for two of these workspaces, with a little slack for names of differing length.
    ketplus::SymbolIndex index(oneWorkspace * 5 / 2);
    index.indexWorkspace(first.path(), listOf({QStringLiteral("a.cpp")}));
    index.indexWorkspace(second.path(), listOf({QStringLiteral("a.cpp")}));
    QCOMPARE(index.workspaceCount(), 2);
    // Touching the first one makes the second the oldest.
    QVERIFY(!index.lookup(first.path(), QStringLiteral("Type1")).isEmpty());

    index.indexWorkspace(third.path(), listOf({QStringLiteral("a.cpp")}));
    QCOMPARE(index.workspaceCount(), 2);
    QVERIFY(index.hasWorkspace(third.path()));
    QVERIFY(index.hasWorkspace(first.path()));
    QVERIFY(!index.hasWorkspace(second.path()));
    QVERIFY(index.memoryBytes() <= index.budgetBytes());
}

void SymbolIndexTest::dropsIdleWorkspaces() {
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid() && second.isValid());
    QVERIFY(writeFile(QDir(first.path()).filePath(QStringLiteral("a.cpp")), "class A {};\n"));
    QVERIFY(writeFile(QDir(second.path()).filePath(QStringLiteral("b.cpp")), "class B {};\n"));

    ketplus::SymbolIndex index;
    index.indexWorkspace(first.path(), listOf({QStringLiteral("a.cpp")}));
    index.indexWorkspace(second.path(), listOf({QStringLiteral("b.cpp")}));
    QTest::qWait(30);

    // Nothing is idle yet at a long threshold, and the kept root is never dropped.
    QCOMPARE(index.evictIdle(60 * 1000, second.path()), 0);
    QCOMPARE(index.evictIdle(10, second.path()), 1);
    QVERIFY(index.hasWorkspace(second.path()));
    QVERIFY(!index.hasWorkspace(first.path()));

    index.clear();
    QCOMPARE(index.workspaceCount(), 0);
    QCOMPARE(index.memoryBytes(), 0);
}

QTEST_MAIN(SymbolIndexTest)
#include "SymbolIndexTest.moc"
