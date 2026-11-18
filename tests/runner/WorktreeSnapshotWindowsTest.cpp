// Worktree snapshots on Windows: the sealed git still ignores hostile hooks, filters and user
// configuration, and a snapshot applies and removes files (read-only ones too) in a workspace.
// WorktreeSnapshotTest covers the same library with POSIX fixtures (symlinks, mode bits, sh
// scripts as git helpers); this one stays on what Windows can express. Skipped elsewhere.
#include "snapshot/SealedGit.h"
#include "snapshot/WorkspaceApply.h"
#include "snapshot/WorkspaceStore.h"
#include "snapshot/WorktreeSnapshot.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace ketplus::runner::snapshot;

namespace {

bool writeFile(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(content) == content.size();
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Plain git for the fixture: a private HOME and no system or global configuration.
bool git(const QString& home, const QString& directory, const QStringList& arguments) {
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("HOME"), home);
    environment.insert(QStringLiteral("USERPROFILE"), home);
    environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
    environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral("NUL"));
    environment.insert(QStringLiteral("GIT_AUTHOR_NAME"), QStringLiteral("Fixture"));
    environment.insert(QStringLiteral("GIT_AUTHOR_EMAIL"), QStringLiteral("f@example.invalid"));
    environment.insert(QStringLiteral("GIT_COMMITTER_NAME"), QStringLiteral("Fixture"));
    environment.insert(QStringLiteral("GIT_COMMITTER_EMAIL"), QStringLiteral("f@example.invalid"));
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory);
    process.start(QStringLiteral("git"), arguments);
    process.closeWriteChannel();
    return process.waitForFinished(60'000) && process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
}

} // namespace

class WorktreeSnapshotWindowsTest : public QObject {
    Q_OBJECT

  private slots:
    void sealedConfigurationUsesTheNullFile() {
#if defined(Q_OS_WIN)
        const QStringList options = SealedGit::sealedOptions();
        QVERIFY(options.contains(QStringLiteral("core.hooksPath=NUL")));
        QVERIFY(options.contains(QStringLiteral("core.attributesFile=NUL")));
        QVERIFY(options.contains(QStringLiteral("core.excludesFile=NUL")));
        const QProcessEnvironment environment = SealedGit().environment();
        QCOMPARE(environment.value(QStringLiteral("GIT_CONFIG_GLOBAL")), QStringLiteral("NUL"));
        QCOMPARE(environment.value(QStringLiteral("GIT_CONFIG_SYSTEM")), QStringLiteral("NUL"));
        QVERIFY(SealedGit().checkVersion().ok());
#else
        QSKIP("Windows only.");
#endif
    }

    void hostileHooksFiltersAndUserConfigurationNeverRun() {
#if defined(Q_OS_WIN)
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString base = root.path();
        const QString home = base + QStringLiteral("/home");
        const QString markers = base + QStringLiteral("/markers");
        QDir().mkpath(home);
        QDir().mkpath(markers);

        // Git for Windows runs these through its own sh, so each one would leave a marker if the
        // sealed git ever started it.
        const QString touch = base + QStringLiteral("/touch.sh");
        QVERIFY(writeFile(touch, QByteArray("#!/bin/sh\ntouch \"") + markers.toUtf8() +
                                     "/$(basename \"$0\")-$$\"\ncat\n"));
        const QString hooks = base + QStringLiteral("/hostile-hooks");
        for (const char* hook : {"pre-commit", "post-commit", "post-index-change",
                                 "reference-transaction", "post-checkout", "pre-auto-gc"}) {
            QVERIFY(writeFile(hooks + QLatin1Char('/') + QString::fromLatin1(hook),
                              QByteArray("#!/bin/sh\ntouch \"") + markers.toUtf8() + "/" + hook +
                                  "\"\n"));
        }

        const QString repo = base + QStringLiteral("/repo");
        QDir().mkpath(repo);
        QVERIFY(git(home, repo, {QStringLiteral("init"), QStringLiteral("-q")}));
        QVERIFY(writeFile(repo + QStringLiteral("/a.txt"), "base\n"));
        QVERIFY(git(home, repo, {QStringLiteral("add"), QStringLiteral("-A")}));
        QVERIFY(git(home, repo,
                    {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("base")}));

        // A hostile repository: an attribute selects a filter its own config defines, and a hook
        // path and an fsmonitor command point at the scripts above.
        QVERIFY(writeFile(repo + QStringLiteral("/.gitattributes"), "*.dat filter=evil\n"));
        const QByteArray filter = QByteArray("[filter \"evil\"]\n\tclean = ") + touch.toUtf8() +
                                  "\n\tsmudge = " + touch.toUtf8() + "\n\trequired = true\n";
        QFile config(repo + QStringLiteral("/.git/config"));
        QVERIFY(config.open(QIODevice::Append));
        config.write(filter + "[core]\n\thooksPath = " + hooks.toUtf8() +
                     "\n\tfsmonitor = " + touch.toUtf8() + "\n");
        config.close();

        // And a hostile user: a global configuration with the same, plus an excludes file.
        QVERIFY(writeFile(home + QStringLiteral("/.gitignore-global"), "globally-ignored.txt\n"));
        const QString globalConfig = home + QStringLiteral("/.gitconfig");
        QVERIFY(writeFile(globalConfig, filter + "[core]\n\thooksPath = " + hooks.toUtf8() +
                                            "\n\tfsmonitor = " + touch.toUtf8() +
                                            "\n\texcludesFile = " + home.toUtf8() +
                                            "/.gitignore-global\n\tautocrlf = true\n"));
        qputenv("HOME", home.toUtf8());
        qputenv("USERPROFILE", home.toUtf8());
        qputenv("GIT_CONFIG_GLOBAL", globalConfig.toUtf8());

        QVERIFY(writeFile(repo + QStringLiteral("/a.txt"), "hello\r\nworld\r\n"));
        QVERIFY(writeFile(repo + QStringLiteral("/globally-ignored.txt"), "hidden\n"));
        QVERIFY(writeFile(repo + QStringLiteral("/data.dat"), "repo filtered\n"));

        const QString store = base + QStringLiteral("/store");
        const auto snapshot = createSnapshot({repo, store});
        QVERIFY2(snapshot.ok(), qPrintable(snapshot.error().message));
        const auto pack = writePack({store, snapshot.value().commit, {}, base + "/p.pack"});
        QVERIFY2(pack.ok(), qPrintable(pack.error().message));
        const QString workspace = base + QStringLiteral("/runner/ws");
        const auto applied = applySnapshot({base + QStringLiteral("/runner/mirror"),
                                            workspace,
                                            pack.value().path,
                                            pack.value().sha256,
                                            snapshot.value().commit,
                                            {}});
        QVERIFY2(applied.ok(), qPrintable(applied.error().message));

        // Nothing ran; contents are the raw bytes (no autocrlf, no filter), the excludes file the
        // user's configuration names is still read as data, and the filtered path is listed but
        // never sent.
        QCOMPARE(QDir(markers).entryList(QDir::Files | QDir::Hidden), QStringList());
        QCOMPARE(readFile(workspace + QStringLiteral("/a.txt")), QByteArray("hello\r\nworld\r\n"));
        QVERIFY(!QFile::exists(workspace + QStringLiteral("/data.dat")));
        QVERIFY(snapshot.value().filteredPaths.contains(QStringLiteral("data.dat")));
        QVERIFY(!QFile::exists(workspace + QStringLiteral("/globally-ignored.txt")));
#else
        QSKIP("Windows only.");
#endif
    }

    void applyingAnOlderSnapshotRemovesDroppedFilesEvenWhenReadOnly() {
#if defined(Q_OS_WIN)
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString base = root.path();
        const QString home = base + QStringLiteral("/home");
        QDir().mkpath(home);
        const QString repo = base + QStringLiteral("/repo");
        QDir().mkpath(repo);
        QVERIFY(git(home, repo, {QStringLiteral("init"), QStringLiteral("-q")}));
        QVERIFY(writeFile(repo + QStringLiteral("/keep.txt"), "keep\n"));
        QVERIFY(writeFile(repo + QStringLiteral("/dir/drop.txt"), "drop\n"));
        const QString store = base + QStringLiteral("/store");
        const auto first = createSnapshot({repo, store});
        QVERIFY2(first.ok(), qPrintable(first.error().message));
        QVERIFY(QFile::remove(repo + QStringLiteral("/dir/drop.txt")));
        QVERIFY(QDir(repo).rmdir(QStringLiteral("dir")));
        QVERIFY(writeFile(repo + QStringLiteral("/keep.txt"), "kept\n"));
        const auto second = createSnapshot({repo, store});
        QVERIFY2(second.ok(), qPrintable(second.error().message));

        const QString mirror = base + QStringLiteral("/runner/mirror");
        const QString workspace = base + QStringLiteral("/runner/ws");
        const auto packOne = writePack({store, first.value().commit, {}, base + "/one.pack"});
        QVERIFY2(packOne.ok(), qPrintable(packOne.error().message));
        QVERIFY2(applySnapshot({mirror,
                                workspace,
                                packOne.value().path,
                                packOne.value().sha256,
                                first.value().commit,
                                {}})
                     .ok(),
                 "first apply");
        const QString dropped = workspace + QStringLiteral("/dir/drop.txt");
        QVERIFY(QFile::exists(dropped));
        // The attribute that stops a plain delete on Windows.
        QVERIFY(QFile::setPermissions(dropped, QFileDevice::ReadOwner));

        const auto packTwo =
            writePack({store, second.value().commit, first.value().commit, base + "/two.pack"});
        QVERIFY2(packTwo.ok(), qPrintable(packTwo.error().message));
        const auto applied = applySnapshot({mirror,
                                            workspace,
                                            packTwo.value().path,
                                            packTwo.value().sha256,
                                            second.value().commit,
                                            {}});
        QVERIFY2(applied.ok(), qPrintable(applied.error().message));
        QVERIFY(!QFile::exists(dropped));
        QCOMPARE(readFile(workspace + QStringLiteral("/keep.txt")), QByteArray("kept\n"));
#else
        QSKIP("Windows only.");
#endif
    }
};

QTEST_GUILESS_MAIN(WorktreeSnapshotWindowsTest)
#include "WorktreeSnapshotWindowsTest.moc"
