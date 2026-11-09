#include "index/SymbolIndex.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <thread>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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
    void followsAFileThatChangedOnDisk();
    void keepsAFileSavedDuringTheFirstBuild();
    void anOlderReadDoesNotOverwriteANewerSave();
};

void SymbolIndexTest::followsAFileThatChangedOnDisk() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir root(directory.path());
    const QString path = root.filePath(QStringLiteral("a.cpp"));
    QVERIFY(writeFile(path, "class Widget {};\n"));

    ketplus::SymbolIndex index;
    index.indexWorkspace(root.absolutePath(), listOf({QStringLiteral("a.cpp")}));
    QCOMPARE(index.lookup(root.absolutePath(), QStringLiteral("Widget")).first().line, 1);

    // Saving moves the declaration; a lookup must not point at the old line.
    QVERIFY(writeFile(path, "// a note\n// another\nclass Widget {};\n"));
    index.reindexFile(root.absolutePath(), QStringLiteral("a.cpp"));
    const auto hits = index.lookup(root.absolutePath(), QStringLiteral("Widget"));
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().line, 3);

    // A declaration that is gone leaves nothing behind.
    QVERIFY(writeFile(path, "int main() { return 0; }\n"));
    index.reindexFile(root.absolutePath(), QStringLiteral("a.cpp"));
    QVERIFY(index.lookup(root.absolutePath(), QStringLiteral("Widget")).isEmpty());
    QCOMPARE(index.fileCount(root.absolutePath()), 1);

    // An unknown workspace is left alone.
    index.reindexFile(QStringLiteral("/nowhere"), QStringLiteral("a.cpp"));
    QCOMPARE(index.workspaceCount(), 1);
}

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

void SymbolIndexTest::keepsAFileSavedDuringTheFirstBuild() {
#if defined(Q_OS_UNIX)
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir root(directory.path());
    const QString saved = root.filePath(QStringLiteral("a.cpp"));
    QVERIFY(writeFile(saved, "class Before {};\n"));
    // Reading the FIFO blocks the build after it has read a.cpp, until the test writes to it.
    const QByteArray gate = QFile::encodeName(root.filePath(QStringLiteral("b.cpp")));
    QCOMPARE(::mkfifo(gate.constData(), 0600), 0);

    ketplus::SymbolIndex index;
    std::thread build([&] {
        index.indexWorkspace(root.absolutePath(),
                             listOf({QStringLiteral("a.cpp"), QStringLiteral("b.cpp")}));
    });
    // A non-blocking writer opens only once the build is waiting on the FIFO. Opened once: a
    // second writer left open would keep the build from ever reading end of file.
    int writer = -1;
    QDeadlineTimer deadline(5000);
    while ((writer = ::open(gate.constData(), O_WRONLY | O_NONBLOCK)) < 0 && !deadline.hasExpired())
        QTest::qWait(10);
    if (writer < 0) {
        // Unblock the build so the thread can be joined.
        writer = ::open(gate.constData(), O_WRONLY);
        ::close(writer);
        build.join();
        QFAIL("the build never reached the FIFO");
    }
    QVERIFY(writeFile(saved, "class After {};\n"));
    index.reindexFile(root.absolutePath(), QStringLiteral("a.cpp"));
    const QByteArray gated("class Gate {};\n");
    QCOMPARE(::write(writer, gated.constData(), static_cast<size_t>(gated.size())),
             static_cast<ssize_t>(gated.size()));
    ::close(writer);
    build.join();

    QCOMPARE(index.lookup(root.absolutePath(), QStringLiteral("After")).size(), 1);
    QVERIFY(index.lookup(root.absolutePath(), QStringLiteral("Before")).isEmpty());
    QCOMPARE(index.lookup(root.absolutePath(), QStringLiteral("Gate")).size(), 1);
#else
    QSKIP("Uses a FIFO to pause the build.");
#endif
}

void SymbolIndexTest::anOlderReadDoesNotOverwriteANewerSave() {
#if defined(Q_OS_UNIX)
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir root(directory.path());
    const QString saved = root.filePath(QStringLiteral("a.cpp"));
    QVERIFY(writeFile(saved, "class Before {};\n"));
    const QByteArray gate = QFile::encodeName(root.filePath(QStringLiteral("b.cpp")));
    QCOMPARE(::mkfifo(gate.constData(), 0600), 0);
    // Opens the FIFO's writer once its reader waits, which pauses the reading thread there.
    const auto writerFor = [](const QByteArray& fifo) {
        int writer = -1;
        QDeadlineTimer deadline(5000);
        while ((writer = ::open(fifo.constData(), O_WRONLY | O_NONBLOCK)) < 0 &&
               !deadline.hasExpired())
            QTest::qWait(10);
        return writer;
    };
    const auto release = [](int writer, const QByteArray& text) {
        const bool written = ::write(writer, text.constData(), static_cast<size_t>(text.size())) ==
                             static_cast<ssize_t>(text.size());
        ::close(writer);
        return written;
    };

    ketplus::SymbolIndex index;
    std::thread build([&] {
        index.indexWorkspace(root.absolutePath(),
                             listOf({QStringLiteral("a.cpp"), QStringLiteral("b.cpp")}));
    });
    const int buildGate = writerFor(gate);
    QVERIFY(buildGate >= 0);
    // A save during the first build is replayed after it publishes; that replay reads a FIFO
    // standing in for the old file, so it is still reading when the next save lands.
    const QByteArray older = QFile::encodeName(saved);
    QVERIFY(QFile::remove(saved));
    QCOMPARE(::mkfifo(older.constData(), 0600), 0);
    index.reindexFile(root.absolutePath(), QStringLiteral("a.cpp"));
    QVERIFY(release(buildGate, "class Gate {};\n"));
    const int replay = writerFor(older);
    QVERIFY(replay >= 0);
    const QString latest = root.filePath(QStringLiteral("latest.tmp"));
    QVERIFY(writeFile(latest, "class Latest {};\n"));
    QCOMPARE(::rename(QFile::encodeName(latest).constData(), older.constData()), 0);
    index.reindexFile(root.absolutePath(), QStringLiteral("a.cpp"));
    QCOMPARE(index.lookup(root.absolutePath(), QStringLiteral("Latest")).size(), 1);
    QVERIFY(release(replay, "class Older {};\n"));
    build.join();

    QCOMPARE(index.lookup(root.absolutePath(), QStringLiteral("Latest")).size(), 1);
    QVERIFY(index.lookup(root.absolutePath(), QStringLiteral("Older")).isEmpty());
#else
    QSKIP("Uses FIFOs to order the reads.");
#endif
}

QTEST_MAIN(SymbolIndexTest)
#include "SymbolIndexTest.moc"
