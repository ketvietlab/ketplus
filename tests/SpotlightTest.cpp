#include "app/RepositoryRouter.h"
#include "app/SpotlightDialog.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <cstdio>

class SpotlightTest final : public QObject {
    Q_OBJECT
    static bool createWorktree(const QString& root, const QString& worktree) {
        const auto git = [](const QStringList& args) {
            QProcess process;
            process.start("git", args);
            return process.waitForFinished(5000) && process.exitCode() == 0;
        };
        return git({"init", "-q", root}) &&
               git({"-C", root, "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                    "commit", "--allow-empty", "-qm", "Initial"}) &&
               git({"-C", root, "worktree", "add", "-qb", "linked", worktree});
    }
    // A `git` that hangs, so every lookup hits the router's 1.5 s timeout.
    static QString slowGitDirectory(const QTemporaryDir& directory) {
        const QString folder = directory.filePath("slow-git");
        QDir().mkpath(folder);
        QFile script(folder + "/git");
        script.open(QIODevice::WriteOnly);
        script.write("#!/bin/sh\n/bin/sleep 5\n");
        script.close();
        script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ExeOwner);
        return folder;
    }
  private slots:
    void initTestCase() {
        qRegisterMetaType<QWidget*>("QWidget*");
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        QCoreApplication::setOrganizationName("KetPlusSpotlightTest");
        QCoreApplication::setApplicationName("KetPlusSpotlightTest");
    }
    void searchesAndActivates() {
        ketplus::ThemeManager theme(*qApp);
        ketplus::SpotlightDialog dialog;
        dialog.setEntries({{"one", "KetPlus.cpp", "/dev/ketplus/src", "Files"},
                           {"two", "Website.md", "/dev/client/website", "Files"},
                           {"missing", "Missing.md", "/old/missing", "Files", false}});
        dialog.show();
        QCOMPARE(dialog.width(), 640);
        QCOMPARE(dialog.height(), 318);
        QCOMPARE(dialog.findChild<QWidget*>(QStringLiteral("spotlightSearchHeader"))->height(), 54);
        QCOMPARE(dialog.findChild<QWidget*>(QStringLiteral("spotlightHint"))->height(), 34);
        QCOMPARE(dialog.findChild<QWidget*>(QStringLiteral("spotlightFooter"))->height(), 40);
        const auto capture = qEnvironmentVariable("KETPLUS_SPOTLIGHT_CAPTURE");
        if (!capture.isEmpty()) {
            QTest::qWait(100);
            QVERIFY(dialog.grab().save(capture));
        }
        auto* query = dialog.findChild<QLineEdit*>("spotlightQuery");
        auto* results = dialog.findChild<QListWidget*>("spotlightResults");
        QVERIFY(query && results);
        QCOMPARE(query->placeholderText(), QStringLiteral("Search files and Atlases…"));
        QCOMPARE(results->count(), 3);
        QCOMPARE(results->item(0)->data(Qt::UserRole + 3).toString(), QStringLiteral("FILES"));
        QVERIFY(results->item(0)->data(Qt::UserRole + 4).toBool());
        QCOMPARE(results->item(0)->sizeHint().height(), 80);
        QCOMPARE(results->item(1)->sizeHint().height(), 54);
        query->setText("CLIENT web");
        QCOMPARE(results->count(), 1);
        QCOMPARE(results->currentItem()->data(Qt::UserRole).toString(), QStringLiteral("two"));
        query->setText("nothing-found");
        QCOMPARE(results->count(), 0);
        query->clear();
        QSignalSpy activated(&dialog, &ketplus::SpotlightDialog::entryActivated);
        QTest::keyClick(query, Qt::Key_Down);
        QTest::keyClick(query, Qt::Key_Return);
        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.first().first().toString(), QStringLiteral("two"));
        QVERIFY(!dialog.isVisible());
        dialog.show();
        QTest::keyClick(query, Qt::Key_Escape);
        QVERIFY(!dialog.isVisible());
    }
    void remembersAndReusesWindow() {
        QTemporaryDir repo;
        QWidget window;
        ketplus::RepositoryRouter router;
        router.registerWindow(repo.path(), &window);
        router.registerWindow(repo.path() + "/.", &window);
        QCOMPARE(ketplus::RepositoryRouter::recentRepositories().count(
                     ketplus::RepositoryRouter::canonicalPath(repo.path())),
                 1);
        QSignalSpy requested(&router, &ketplus::RepositoryRouter::openRequested);
        router.activate(repo.path());
        QVERIFY(window.isVisible());
        QCOMPARE(requested.count(), 0);
        window.hide();
        ketplus::RepositoryRouter remote;
        QSignalSpy remoteOpen(&remote, &ketplus::RepositoryRouter::openRequested);
        QSignalSpy errors(&remote, &ketplus::RepositoryRouter::activationFailed);
        remote.activate(repo.path());
        QTRY_VERIFY(window.isVisible());
        QTest::qWait(100);
        QCOMPARE(remoteOpen.count(), 0);
        QCOMPARE(errors.count(), 0);
    }
    void dropsOversizedRoutingRequests() {
        QTemporaryDir repo;
        QWidget window;
        ketplus::RepositoryRouter router;
        router.registerWindow(repo.path(), &window);
        QLocalSocket client;
        client.connectToServer(ketplus::RepositoryRouter::endpointName(repo.path()));
        QVERIFY(client.waitForConnected(1000));
        // A client that never ends its line is cut off instead of buffering until the timeout.
        client.write(QByteArray(32 * 1024, 'a'));
        QVERIFY(client.waitForBytesWritten(1000));
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QLocalSocket::UnconnectedState, 600);
        QVERIFY(!window.isVisible());
    }

    void timeoutRoutesToTheCachedCheckout() {
        QTemporaryDir directory;
        const auto root = directory.filePath("project");
        const auto linked = directory.filePath("project-feature");
        QVERIFY(createWorktree(root, linked));
        QVERIFY(QDir().mkpath(linked + "/src"));
        QFile file(linked + "/src/file.txt");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("text\n");
        file.close();
        QWidget window;
        ketplus::RepositoryRouter router;
        router.registerWindow(linked, &window);
        QCOMPARE(ketplus::RepositoryRouter::repositoryRoot(linked + "/src"),
                 ketplus::RepositoryRouter::canonicalPath(root));
        QSignalSpy worktrees(&router, &ketplus::RepositoryRouter::worktreeRequested);
        QSignalSpy files(&router, &ketplus::RepositoryRouter::fileRequested);
        QSignalSpy opened(&router, &ketplus::RepositoryRouter::openFileRequested);
        const auto oldPath = qgetenv("PATH");
        qputenv("PATH", slowGitDirectory(directory).toUtf8());
        router.openFile(file.fileName());
        qputenv("PATH", oldPath);
        // The window stays on its checkout instead of switching into `src`.
        QCOMPARE(files.count(), 1);
        QCOMPARE(worktrees.count(), 0);
        QCOMPARE(opened.count(), 0);
    }

    void dropsDataAfterTheRoutingRequest() {
        QTemporaryDir repo;
        QTemporaryDir directory;
        QWidget window;
        ketplus::RepositoryRouter router;
        router.registerWindow(repo.path(), &window);
        QLocalSocket client;
        client.connectToServer(ketplus::RepositoryRouter::endpointName(repo.path()));
        QVERIFY(client.waitForConnected(1000));
        const auto oldPath = qgetenv("PATH");
        qputenv("PATH", slowGitDirectory(directory).toUtf8());
        const QByteArray request =
            QJsonDocument(QJsonObject{{"action", "activate"}, {"path", repo.path()}})
                .toJson(QJsonDocument::Compact) +
            '\n';
        client.write(request + QByteArray(512 * 1024, 'a'));
        // Cut off well before the 1.5 s lookup could answer, without buffering the rest.
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QLocalSocket::UnconnectedState, 1000);
        for (auto* socket : router.findChildren<QLocalSocket*>())
            QVERIFY(socket->bytesAvailable() <= 16 * 1024 + 1);
        QTest::qWait(1700);
        qputenv("PATH", oldPath);
        QVERIFY(!window.isVisible());
    }

    void registersWindowOffTheGuiThread() {
        QTemporaryDir first;
        QTemporaryDir second;
        QWidget window;
        ketplus::RepositoryRouter router;
        QSignalSpy requested(&router, &ketplus::RepositoryRouter::openRequested);
        // The later folder wins even if the earlier lookup lands after it.
        router.registerWindowLater(first.path(), &window);
        router.registerWindowLater(second.path(), &window);
        QTRY_VERIFY(ketplus::RepositoryRouter::recentRepositories().contains(
            ketplus::RepositoryRouter::canonicalPath(second.path())));
        router.activate(second.path());
        QVERIFY(window.isVisible());
        QCOMPARE(requested.count(), 0);
        window.hide();
        router.activate(first.path());
        QTRY_COMPARE(requested.count(), 1);
        QVERIFY(!window.isVisible());
        QCOMPARE(requested.first().first().toString(),
                 ketplus::RepositoryRouter::canonicalPath(first.path()));
        // A window closed before its lookup lands is never registered.
        auto* closing = new QWidget;
        router.registerWindowLater(first.path(), closing);
        delete closing;
        QTest::qWait(200);
        requested.clear();
        router.activate(first.path());
        QTRY_COMPARE(requested.count(), 1);
    }
    void searchesOnlyFilesAndAtlases() {
        ketplus::SpotlightDialog dialog;
        dialog.setEntries({{"file", "main.cpp", "/worktrees/pos/src", "Files", true, "file"},
                           {"atlas", "Checkout", "/worktrees/pos/docs/atlas.json", "Atlases", true,
                            "atlas", "/other/linked-checkout/docs/atlas.json"}});
        auto* query = dialog.findChild<QLineEdit*>("spotlightQuery");
        auto* results = dialog.findChild<QListWidget*>("spotlightResults");
        QCOMPARE(results->count(), 2);
        QCOMPARE(results->item(1)->data(Qt::UserRole + 3).toString(), QStringLiteral("ATLAS"));
        query->setText("linked-checkout");
        QCOMPARE(results->count(), 1);
        QCOMPARE(results->currentItem()->data(Qt::UserRole).toString(), "atlas");
        // Repository and worktree scopes are gone; '@' has no special meaning.
        query->setText("@repo");
        QCOMPARE(results->count(), 0);
        QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("spotlightAnywhere"))->text(),
                 QStringLiteral("FILES & ATLASES"));
        QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("spotlightScope")) == nullptr);
    }
    void recentRepositoriesCollapseLinkedWorktrees() {
        QTemporaryDir directory;
        const auto root = directory.filePath("project");
        const auto linked = directory.filePath("project-feature");
        QVERIFY(createWorktree(root, linked));
        QSettings settings;
        settings.setValue("spotlight/repositories", QStringList{linked, root});
        settings.setValue("workspace/recentFolders", QStringList{linked});
        const auto repos = ketplus::RepositoryRouter::recentRepositories();
        QCOMPARE(repos, QStringList{ketplus::RepositoryRouter::canonicalPath(root)});
        QCOMPARE(ketplus::RepositoryRouter::repositoryRoot(linked), repos.first());
        // Cached navigation reads survive Git being unavailable and retain deduplication.
        const auto oldPath = qgetenv("PATH");
        qputenv("PATH", directory.filePath("no-executables").toUtf8());
        const auto cachedRoot = ketplus::RepositoryRouter::cachedRepositoryRoot(linked);
        const auto cachedRepos = ketplus::RepositoryRouter::cachedRecentRepositories();
        qputenv("PATH", oldPath);
        QCOMPARE(cachedRoot, repos.first());
        QCOMPARE(cachedRepos, repos);
    }
    void opensClosedAndRejectsMissing() {
        QTemporaryDir repo;
        ketplus::RepositoryRouter router;
        QSignalSpy requested(&router, &ketplus::RepositoryRouter::openRequested);
        QSignalSpy errors(&router, &ketplus::RepositoryRouter::activationFailed);
        router.activate(repo.path());
        QTRY_COMPARE(requested.count(), 1);
        QCOMPARE(requested.first().first().toString(),
                 ketplus::RepositoryRouter::canonicalPath(repo.path()));
        router.activate(repo.filePath("missing"));
        QCOMPARE(errors.count(), 1);
        QCOMPARE(requested.count(), 1);
    }
    void linkedWorktreeUsesSameWindow() {
        QTemporaryDir directory;
        const auto root = directory.filePath("repo");
        const auto linked = directory.filePath("linked worktree");
        QVERIFY(createWorktree(root, linked));
        QCOMPARE(ketplus::RepositoryRouter::endpointName(root),
                 ketplus::RepositoryRouter::endpointName(linked));
        QWidget window;
        ketplus::RepositoryRouter router;
        router.registerWindow(root, &window);
        QSignalSpy opened(&router, &ketplus::RepositoryRouter::openRequested);
        QSignalSpy switched(&router, &ketplus::RepositoryRouter::worktreeRequested);
        connect(&router, &ketplus::RepositoryRouter::worktreeRequested, &router,
                [&router](QWidget* target, const QString& path) {
                    router.registerWindow(path, target);
                });
        router.activate(linked);
        QCOMPARE(opened.count(), 0);
        QCOMPARE(switched.count(), 1);
        QCOMPARE(switched.first().at(0).value<QWidget*>(), &window);
        QCOMPARE(switched.first().at(1).toString(),
                 ketplus::RepositoryRouter::canonicalPath(linked));
        router.activate(linked);
        QCOMPARE(switched.count(), 1);
        router.activate(root);
        QCOMPARE(switched.count(), 2);
        QCOMPARE(opened.count(), 0);
    }
    void routesFileToLinkedWorktree() {
        QTemporaryDir directory;
        const auto root = directory.filePath("repo");
        const auto linked = directory.filePath("linked worktree");
        QVERIFY(createWorktree(root, linked));
        const QString filePath = QDir(linked).filePath(QStringLiteral("external.cpp"));
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(file.write("int external = 1;\n") > 0);
        file.close();

        QWidget window;
        ketplus::RepositoryRouter router;
        router.registerWindow(root, &window);
        QSignalSpy switched(&router, &ketplus::RepositoryRouter::worktreeRequested);
        QSignalSpy opened(&router, &ketplus::RepositoryRouter::fileRequested);
        QSignalSpy diffed(&router, &ketplus::RepositoryRouter::fileDiffRequested);
        connect(&router, &ketplus::RepositoryRouter::worktreeRequested, &router,
                [&router](QWidget* target, const QString& path) {
                    router.registerWindow(path, target);
                });

        router.openFile(filePath);
        QCOMPARE(switched.count(), 1);
        QCOMPARE(switched.constFirst().at(1).toString(),
                 ketplus::RepositoryRouter::canonicalPath(linked));
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.constFirst().at(0).value<QWidget*>(), &window);
        QCOMPARE(opened.constFirst().at(1).toString(),
                 ketplus::RepositoryRouter::canonicalPath(filePath));
        QVERIFY(window.isVisible());

        router.openFileDiff(filePath);
        QCOMPARE(switched.count(), 1);
        QCOMPARE(diffed.count(), 1);
        QCOMPARE(diffed.constFirst().at(0).value<QWidget*>(), &window);
        QCOMPARE(diffed.constFirst().at(1).toString(),
                 ketplus::RepositoryRouter::canonicalPath(filePath));
    }
    void opensWindowForFileWhenClosed() {
        QTemporaryDir directory;
        const auto root = directory.filePath("repo");
        const auto linked = directory.filePath("linked worktree");
        QVERIFY(createWorktree(root, linked));
        const QString filePath = QDir(linked).filePath(QStringLiteral("external.cpp"));
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(file.write("int external = 1;\n") > 0);
        file.close();

        ketplus::RepositoryRouter router;
        QSignalSpy opened(&router, &ketplus::RepositoryRouter::openFileRequested);
        QSignalSpy diffed(&router, &ketplus::RepositoryRouter::openFileDiffRequested);
        QSignalSpy errors(&router, &ketplus::RepositoryRouter::activationFailed);
        router.openFile(filePath);
        QTRY_COMPARE(opened.count(), 1);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(opened.constFirst().at(0).toString(),
                 ketplus::RepositoryRouter::canonicalPath(linked));
        QCOMPARE(opened.constFirst().at(1).toString(),
                 ketplus::RepositoryRouter::canonicalPath(filePath));

        router.openFileDiff(filePath);
        QTRY_COMPARE(diffed.count(), 1);
        QCOMPARE(diffed.constFirst().at(0).toString(),
                 ketplus::RepositoryRouter::canonicalPath(linked));
        QCOMPARE(diffed.constFirst().at(1).toString(),
                 ketplus::RepositoryRouter::canonicalPath(filePath));
    }
    void servesPeer() {
        const auto path = qEnvironmentVariable("KETPLUS_TEST_REPOSITORY");
        if (path.isEmpty())
            QSKIP("Only used by the cross-process test");
        QWidget window;
        ketplus::RepositoryRouter router;
        router.registerWindow(path, &window);
        QSignalSpy switched(&router, &ketplus::RepositoryRouter::worktreeRequested);
        QSignalSpy files(&router, &ketplus::RepositoryRouter::fileRequested);
        QSignalSpy diffs(&router, &ketplus::RepositoryRouter::fileDiffRequested);
        connect(&router, &ketplus::RepositoryRouter::worktreeRequested, &router,
                [&router](QWidget* target, const QString& worktree) {
                    router.registerWindow(worktree, target);
                });
        std::puts("READY");
        std::fflush(stdout);
        QTRY_VERIFY_WITH_TIMEOUT(window.isVisible(), 5000);
        const auto expected = qEnvironmentVariable("KETPLUS_TEST_WORKTREE");
        if (!expected.isEmpty()) {
            QCOMPARE(switched.count(), 1);
            QCOMPARE(switched.first().at(1).toString(),
                     ketplus::RepositoryRouter::canonicalPath(expected));
        }
        const auto expectedFile = qEnvironmentVariable("KETPLUS_TEST_FILE");
        if (!expectedFile.isEmpty()) {
            const bool expectsDiff = qEnvironmentVariableIsSet("KETPLUS_TEST_FILE_DIFF");
            const QSignalSpy& routed = expectsDiff ? diffs : files;
            QCOMPARE(routed.count(), 1);
            QCOMPARE(routed.constFirst().at(1).toString(),
                     ketplus::RepositoryRouter::canonicalPath(expectedFile));
        }
        QTest::qWait(100);
    }
    void activatesOtherProcess() {
        QTemporaryDir repo;
        const auto root = repo.filePath("main");
        const auto linked = repo.filePath("linked");
        QVERIFY(createWorktree(root, linked));
        QProcess peer;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("KETPLUS_TEST_REPOSITORY", root);
        environment.insert("KETPLUS_TEST_WORKTREE", linked);
        peer.setProcessEnvironment(environment);
        peer.start(QCoreApplication::applicationFilePath(), {"servesPeer"});
        QVERIFY(peer.waitForStarted());
        QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output += peer.readAllStandardOutput()).contains("READY"), 5000);
        ketplus::RepositoryRouter router;
        QSignalSpy opened(&router, &ketplus::RepositoryRouter::openRequested);
        QSignalSpy errors(&router, &ketplus::RepositoryRouter::activationFailed);
        router.activate(linked);
        QTRY_COMPARE_WITH_TIMEOUT(peer.state(), QProcess::NotRunning, 5000);
        QCOMPARE(peer.exitCode(), 0);
        QCOMPARE(opened.count(), 0);
        QCOMPARE(errors.count(), 0);
    }
    void opensFileInOtherProcess() {
        QTemporaryDir repo;
        const auto root = repo.filePath("main");
        const auto linked = repo.filePath("linked");
        QVERIFY(createWorktree(root, linked));
        const QString filePath = QDir(linked).filePath(QStringLiteral("external.cpp"));
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(file.write("int external = 1;\n") > 0);
        file.close();

        QProcess peer;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("KETPLUS_TEST_REPOSITORY", root);
        environment.insert("KETPLUS_TEST_WORKTREE", linked);
        environment.insert("KETPLUS_TEST_FILE", filePath);
        peer.setProcessEnvironment(environment);
        peer.start(QCoreApplication::applicationFilePath(), {"servesPeer"});
        QVERIFY(peer.waitForStarted());
        QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output += peer.readAllStandardOutput()).contains("READY"), 5000);
        ketplus::RepositoryRouter router;
        QSignalSpy opened(&router, &ketplus::RepositoryRouter::openFileRequested);
        QSignalSpy errors(&router, &ketplus::RepositoryRouter::activationFailed);
        router.openFile(filePath);
        QTRY_COMPARE_WITH_TIMEOUT(peer.state(), QProcess::NotRunning, 5000);
        QCOMPARE(peer.exitCode(), 0);
        QCOMPARE(opened.count(), 0);
        QCOMPARE(errors.count(), 0);
    }
    void opensFileDiffInOtherProcess() {
        QTemporaryDir repo;
        const auto root = repo.filePath("main");
        const auto linked = repo.filePath("linked");
        QVERIFY(createWorktree(root, linked));
        const QString filePath = QDir(linked).filePath(QStringLiteral("external.cpp"));
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(file.write("int external = 1;\n") > 0);
        file.close();

        QProcess peer;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("KETPLUS_TEST_REPOSITORY", root);
        environment.insert("KETPLUS_TEST_WORKTREE", linked);
        environment.insert("KETPLUS_TEST_FILE", filePath);
        environment.insert("KETPLUS_TEST_FILE_DIFF", "1");
        peer.setProcessEnvironment(environment);
        peer.start(QCoreApplication::applicationFilePath(), {"servesPeer"});
        QVERIFY(peer.waitForStarted());
        QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output += peer.readAllStandardOutput()).contains("READY"), 5000);
        ketplus::RepositoryRouter router;
        QSignalSpy opened(&router, &ketplus::RepositoryRouter::openFileDiffRequested);
        QSignalSpy errors(&router, &ketplus::RepositoryRouter::activationFailed);
        router.openFileDiff(filePath);
        QTRY_COMPARE_WITH_TIMEOUT(peer.state(), QProcess::NotRunning, 5000);
        QCOMPARE(peer.exitCode(), 0);
        QCOMPARE(opened.count(), 0);
        QCOMPARE(errors.count(), 0);
    }

  private:
    QTemporaryDir settings_;
};

QTEST_MAIN(SpotlightTest)
#include "SpotlightTest.moc"
