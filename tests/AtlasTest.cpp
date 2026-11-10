#include "atlas/AtlasTab.h"
#include "atlas/AtlasThumbnailCache.h"
#include "ui/Theme.h"
#include "workspace/AtlasIndex.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QWebView>
#include <QtTest>
#include <QtWebView/QtWebView>

class AtlasTest final : public QObject {
    Q_OBJECT
    static bool write(const QString& path, const QByteArray& data) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
    }
    static QByteArray config() {
        return R"({"version":1,"title":"Checkout","screens":[{"id":"home","title":"Home","url":"./screen.html"}],"flows":[{"id":"checkout","title":"Checkout","nodes":[{"id":"entry","screen":"home"}],"edges":[]}]})";
    }
    static ketplus::AtlasDescriptor
    fixture(const QString& workspace, const QString& name = QStringLiteral("checkout.ketatlas")) {
        const QString directory = QDir(workspace).filePath(name);
        write(directory + "/atlas.json", config());
        write(directory + "/screen.html",
              "<!doctype html><html><body style='background:#16a34a;color:white'><h1>Atlas "
              "integration</h1><button "
              "onclick='this.textContent=\"Clicked\"'>Test</button></body></html>");
        return {directory + "/atlas.json",
                workspace,
                directory,
                name + "/atlas.json",
                "Checkout",
                {{"home", "Home", "./screen.html", directory + "/screen.html", false}},
                1,
                0,
                0,
                {},
                {}};
    }
  private slots:
    void explicitManifestIsBoundedAndWorktreeScoped() {
        QTemporaryDir workspace;
        QTemporaryDir outside;
        const auto atlas = fixture(workspace.path());
        const auto actual = ketplus::AtlasIndex::readManifest(atlas.path, workspace.path());
        QCOMPARE(actual.title, QString("Checkout"));
        QCOMPARE(actual.screens.size(), 1);
        QCOMPARE(actual.flowCount, 1);
        QVERIFY(ketplus::AtlasIndex::readManifest(atlas.path, outside.path()).path.isEmpty());
        const auto foreign = fixture(outside.path());
        QVERIFY(QFile::link(QFileInfo(foreign.path).absolutePath(),
                            workspace.filePath("link.ketatlas")));
        QVERIFY(ketplus::AtlasIndex::readManifest(workspace.filePath("link.ketatlas/atlas.json"),
                                                  workspace.path())
                    .path.isEmpty());
        QVERIFY(write(atlas.path, "{invalid"));
        QVERIFY(ketplus::AtlasIndex::readManifest(atlas.path, workspace.path()).path.isEmpty());
        QVERIFY(write(atlas.path, QByteArray(ketplus::AtlasIndex::maximumManifestBytes + 1, ' ')));
        QVERIFY(ketplus::AtlasIndex::readManifest(atlas.path, workspace.path()).path.isEmpty());
        QVERIFY(QFile::remove(atlas.path));
        QVERIFY(ketplus::AtlasIndex::readManifest(atlas.path, workspace.path()).path.isEmpty());
    }
    void processReadinessMatchesAttempt_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("valid");
        for (const auto& kind : {"wrong-port", "invalid-port", "overflow-port", "embedded",
                                 "partial-longer-port", "truncated-prefix", "stop-banner"})
            QTest::newRow(kind) << QString(kind) << false;
        for (const auto& kind : {"wrong-then-right", "lf", "crlf", "slash", "split-valid"})
            QTest::newRow(kind) << QString(kind) << true;
    }
    void processReadinessMatchesAttempt() {
#ifndef Q_OS_UNIX
        QSKIP("The synthetic CLI fixture uses a Unix shell.");
#else
        QFETCH(QString, kind);
        QFETCH(bool, valid);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto atlas = fixture(root.path());
        const auto cli = root.filePath("fake-atlas");
        QFile source(QFINDTESTDATA("fixtures/fake-atlas-readiness.sh"));
        QVERIFY(source.open(QIODevice::ReadOnly));
        auto script = source.readAll();
        script.replace("@MODE@", kind.toUtf8());
        QVERIFY(write(cli, script));
        QVERIFY(QFile::setPermissions(cli, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                               QFileDevice::ExeOwner));
        const auto previous = qgetenv("KETPLUS_KETATLAS_CLI");
        const auto restore = qScopeGuard([&] {
            previous.isNull() ? qunsetenv("KETPLUS_KETATLAS_CLI")
                              : qputenv("KETPLUS_KETATLAS_CLI", previous);
        });
        qputenv("KETPLUS_KETATLAS_CLI", cli.toUtf8());
        ketplus::AtlasProcess process;
        QSignalSpy ready(&process, &ketplus::AtlasProcess::ready);
        QSignalSpy failed(&process, &ketplus::AtlasProcess::failed);
        process.start(atlas);
        if (kind == "stop-banner") {
            QTRY_VERIFY_WITH_TIMEOUT(
                QFileInfo::exists(QDir(atlas.directory).filePath("stop-ready")), 3000);
            process.stop();
            QVERIFY(!process.running());
            QVERIFY(ready.isEmpty());
            QVERIFY(failed.isEmpty());
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 3000);
        QCOMPARE(!ready.isEmpty(), valid);
        if (!valid) {
            QCOMPARE(failed.size(), 1);
            // A truncated prefix from the failed attempt must not poison restart.
            script.replace("case " + kind.toUtf8() + " in", "case lf in");
            QVERIFY(write(cli, script));
            failed.clear();
            process.start(atlas);
            QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 3000);
        }
        QCOMPARE(ready.size(), 1);
        QVERIFY(failed.isEmpty());
        QFile portFile(QDir(atlas.directory).filePath("attempted-port"));
        QVERIFY(portFile.open(QIODevice::ReadOnly));
        const auto expectedPort = portFile.read(16).toInt();
        QVERIFY(expectedPort > 0 && expectedPort <= 65535);
        const auto url = ready.first().first().toUrl();
        QCOMPARE(url.scheme(), QString("http"));
        QCOMPARE(url.host(), QString("127.0.0.1"));
        QCOMPARE(url.port(), expectedPort);
        process.stop();
        QVERIFY(!process.running());
#endif
    }

    void newestNodeVersionIsSearchedFirst() {
        if (QFileInfo::exists("/opt/homebrew/bin/ketatlas") ||
            QFileInfo::exists("/usr/local/bin/ketatlas"))
            QSKIP("a system ketatlas is found before nvm");
        QTemporaryDir home;
        QVERIFY(home.isValid());
        for (const auto* version : {"v9.11.2", "v26.7.0", "v26.12.1", "v24.14.1"}) {
            const auto cli =
                home.filePath(QString(".nvm/versions/node/%1/bin/ketatlas").arg(version));
            QVERIFY(QDir().mkpath(QFileInfo(cli).absolutePath()));
            QVERIFY(write(cli, "#!/bin/sh\n"));
            QVERIFY(QFile::setPermissions(cli, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        }
        const auto previousHome = qgetenv("HOME");
        const auto previousPath = qgetenv("PATH");
        const auto previousCli = qgetenv("KETPLUS_KETATLAS_CLI");
        const auto restore = qScopeGuard([&] {
            qputenv("HOME", previousHome);
            qputenv("PATH", previousPath);
            previousCli.isNull() ? qunsetenv("KETPLUS_KETATLAS_CLI")
                                 : qputenv("KETPLUS_KETATLAS_CLI", previousCli);
        });
        qputenv("HOME", home.path().toUtf8());
        qputenv("PATH", home.filePath("empty").toUtf8());
        qunsetenv("KETPLUS_KETATLAS_CLI");
        QSettings().remove("atlas/cliPath");
        QCOMPARE(QFileInfo(ketplus::AtlasProcess::cliPath()).canonicalFilePath(),
                 QFileInfo(home.filePath(".nvm/versions/node/v26.12.1/bin/ketatlas"))
                     .canonicalFilePath());
    }
    void boundsTheWalkOutsideGit() {
        QTemporaryDir root;
        write(root.filePath(".hidden/secret.ketatlas/atlas.json"), config());
        fixture(root.path());
        bool truncated = true;
        QCOMPARE(ketplus::AtlasIndex::scan(root.path(), {}, &truncated).size(), 1);
        QVERIFY(!truncated);

        // More folders than the walk visits, sorted after the bundle so the walk reaches them.
        for (int index = 0; index < ketplus::AtlasIndex::maximumScannedDirectories; ++index)
            QVERIFY(QDir(root.path()).mkpath(QStringLiteral("zz/d%1").arg(index)));
        QElapsedTimer elapsed;
        elapsed.start();
        (void)ketplus::AtlasIndex::scan(root.path(), {}, &truncated);
        QVERIFY(truncated);
        QVERIFY(elapsed.elapsed() < ketplus::AtlasIndex::maximumScanMilliseconds + 1'000);
    }
    void indexesOnlyAtlasManifests() {
        QTemporaryDir root;
        fixture(root.path());
        write(root.filePath("src/main.cpp"), "int main() {}\n");
        write(root.filePath("node_modules/hidden.ketatlas/atlas.json"), config());
        write(root.filePath(".git/hidden.ketatlas/atlas.json"), config());
        write(root.filePath("nested/photo.PNG"), "not even an image");
        write(root.filePath("unknown.bin"), QByteArray("a\0b", 3));
        write(root.filePath("other.json"), "{\"version\":1}");
        write(root.filePath("legacy/atlas.json"), config());
        write(root.filePath("tests/baseline.ketatlas/atlas.json"), config());
        const auto manifests = ketplus::AtlasIndex::scan(root.path());
        QCOMPARE(manifests.size(), 2);
        QCOMPARE(ketplus::AtlasIndex::visibleAtlases(manifests).size(), 1);
        const auto atlas = ketplus::AtlasIndex::visibleAtlases(manifests).first();
        QCOMPARE(atlas.title, QStringLiteral("Checkout"));
        QCOMPARE(atlas.screens.size(), 1);
        QCOMPARE(atlas.screens.first().id, QStringLiteral("home"));
        QCOMPARE(atlas.screens.first().localPath,
                 QFileInfo(root.filePath("checkout.ketatlas/screen.html")).canonicalFilePath());
        QVERIFY(!atlas.screens.first().remote);
        QCOMPARE(atlas.flows.size(), 1);
        QCOMPARE(atlas.flows.first().id, QStringLiteral("checkout"));
        write(root.filePath("checkout.ketatlas/atlas.json"), "{\"invalid\":true}");
        const auto changed = ketplus::AtlasIndex::scan(root.path());
        QCOMPARE(ketplus::AtlasIndex::visibleAtlases(changed).size(), 0);
    }
    void scansOnlySelectedWorktree() {
        QTemporaryDir root;
        const auto main = root.filePath("main");
        const auto linked = root.filePath("linked tree");
        const auto git = [](const QStringList& args) {
            QProcess process;
            process.start("git", args);
            return process.waitForFinished(5000) && process.exitCode() == 0;
        };
        QVERIFY(git({"init", "-q", main}));
        QVERIFY(git({"-C", main, "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                     "commit", "--allow-empty", "-qm", "Initial"}));
        QVERIFY(git({"-C", main, "worktree", "add", "-qb", "linked", linked}));
        fixture(main);
        fixture(linked);
        write(linked + "/checkout.ketatlas/atlas.json",
              QByteArray(config()).replace("Checkout", "Changed"));

        const auto mainAtlases = ketplus::AtlasIndex::scan(main);
        QCOMPARE(ketplus::AtlasIndex::visibleAtlases(mainAtlases).size(), 1);
        QCOMPARE(ketplus::AtlasIndex::visibleAtlases(mainAtlases).first().workspaceRoot,
                 QFileInfo(main).canonicalFilePath());
        QVERIFY(
            std::none_of(mainAtlases.cbegin(), mainAtlases.cend(),
                         [&linked](const auto& atlas) { return atlas.path.startsWith(linked); }));

        ketplus::AtlasIndex index;
        index.setRoot(main);
        QTRY_COMPARE(index.workspaceRoot(), QFileInfo(main).canonicalFilePath());
        QTRY_COMPARE(index.atlases().size(), 1);
        ketplus::AtlasTab tab(&index);
        auto* grid = tab.findChild<QListWidget*>("atlasGrid");
        QCOMPARE(grid->count(), 1);
        QCOMPARE(grid->item(0)->data(Qt::UserRole).toString(),
                 QFileInfo(main).canonicalFilePath() + "/checkout.ketatlas/atlas.json");
        // The library names the folder it lists.
        auto* scope = tab.findChild<QLabel*>("atlasScope");
        QVERIFY(scope);
        QCOMPARE(scope->text(), QStringLiteral("main"));

        index.setRoot(linked);
        QVERIFY(index.manifests().isEmpty());
        QTRY_COMPARE(index.workspaceRoot(), QFileInfo(linked).canonicalFilePath());
        QCOMPARE(scope->text(), QStringLiteral("linked tree"));
        QTRY_COMPARE(grid->count(), 1);
        QCOMPARE(grid->item(0)->data(Qt::UserRole).toString(),
                 QFileInfo(linked).canonicalFilePath() + "/checkout.ketatlas/atlas.json");
    }
    void discoversManifestCreatedBeforeScanCompletionIsDelivered() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ketplus::AtlasIndex index;
        index.setRoot(root.path());
        index.refresh();
        // Let the empty scan finish without dispatching its queued completion.
        // The manifest's filesystem event must survive applying that snapshot.
        QElapsedTimer deadline;
        deadline.start();
        while (index.isIndexing() && deadline.elapsed() < 5000)
            QThread::msleep(1);
        QVERIFY(!index.isIndexing());
        fixture(root.path());
        QTRY_COMPARE_WITH_TIMEOUT(index.atlases().size(), 1, 5000);
    }

    void refreshesManifestChanges() {
        QTemporaryDir root;
        ketplus::AtlasIndex index;
        index.setRoot(root.path());
        QTRY_VERIFY(!index.isIndexing());
        QCOMPARE(index.atlases().size(), 0);
        ketplus::AtlasTab tab(&index);
        tab.resize(800, 600);
        tab.show();
        auto* search = tab.findChild<QLineEdit*>("atlasSearch");
        auto* grid = tab.findChild<QListWidget*>("atlasGrid");
        auto* count = tab.findChild<QLabel*>("atlasCount");
        auto* empty = tab.findChild<QLabel*>("atlasEmptyState");
        QVERIFY(search && grid && count && empty);
        QVERIFY(search->isHidden());
        QVERIFY(grid->isHidden());
        QVERIFY(count->isHidden());
        QVERIFY(empty->isVisible());

        QSignalSpy unchanged(&index, &ketplus::AtlasIndex::changed);
        index.refresh();
        QTRY_VERIFY(!index.isIndexing());
        QCOMPARE(unchanged.count(), 0);

        fixture(root.path());
        QTRY_COMPARE_WITH_TIMEOUT(index.atlases().size(), 1, 5000);
        QVERIFY(search->isVisible());
        QVERIFY(grid->isVisible());
        QVERIFY(count->isVisible());
        QVERIFY(empty->isHidden());
        write(root.filePath("checkout.ketatlas/atlas.json"),
              QByteArray(config()).replace("Checkout", "Updated!"));
        QTRY_COMPARE_WITH_TIMEOUT(index.atlases().first().title, QStringLiteral("Updated!"), 5000);
        QVERIFY(QFile::remove(root.filePath("checkout.ketatlas/atlas.json")));
        QTRY_COMPARE_WITH_TIMEOUT(index.atlases().size(), 0, 5000);
    }
    void preservesCreationEventAcrossScanCompletion() {
        QTemporaryDir root;
        ketplus::AtlasIndex index;
        index.setRoot(root.path());
        index.refresh();
        // Finish the empty scan without delivering its queued completion slot.
        QElapsedTimer deadline;
        deadline.start();
        while (index.isIndexing() && deadline.elapsed() < 5000)
            QTest::qSleep(1);
        QVERIFY(!index.isIndexing());
        fixture(root.path());
        // Completion must not remove the root watch and discard this event.
        QTRY_COMPARE_WITH_TIMEOUT(index.atlases().size(), 1, 5000);
    }

    void gridSearchAndDeferKeepsTab() {
        ketplus::ThemeManager theme(*qApp);
        theme.setMode(ketplus::ThemeManager::Mode::Light);
        QTemporaryDir root;
        const auto atlas = fixture(root.path());
        ketplus::AtlasIndex index;
        index.setRoot(root.path());
        QTRY_VERIFY(!index.manifests().isEmpty());
        ketplus::AtlasTab tab(&index);
        tab.resize(800, 600);
        tab.show();
        auto* grid = tab.findChild<QListWidget*>("atlasGrid");
        auto* search = tab.findChild<QLineEdit*>("atlasSearch");
        auto* gridMode = tab.findChild<QAbstractButton*>("atlasGridModeButton");
        auto* listMode = tab.findChild<QAbstractButton*>("atlasListModeButton");
        QVERIFY(gridMode && listMode);
        QVERIFY(gridMode->isChecked());
        QCOMPARE(grid->gridSize().height(), 300);
        QVERIFY(
            tab.findChildren<QAbstractButton*>(QString{}, Qt::FindDirectChildrenOnly).isEmpty());
        QCOMPARE(grid->count(), 1);
        QTRY_VERIFY(!grid->item(0)->data(Qt::DecorationRole).value<QPixmap>().isNull());
        search->setText("missing");
        QCOMPARE(grid->count(), 0);
        search->setText("checkout atlas.json");
        QCOMPARE(grid->count(), 1);
        listMode->click();
        QVERIFY(listMode->isChecked());
        QCOMPARE(grid->gridSize().height(), 112);
        QVERIFY(grid->gridSize().width() >= grid->viewport()->width() - 12);
        gridMode->click();
        QVERIFY(gridMode->isChecked());
        QSignalSpy selected(&tab, &ketplus::AtlasTab::atlasRequested);
        grid->setCurrentRow(0);
        QTest::qWait(100);
        const auto gridCapture = qEnvironmentVariable("KETPLUS_ATLAS_GRID_CAPTURE");
        if (!gridCapture.isEmpty())
            QVERIFY(tab.grab().save(gridCapture));
        QTest::mouseClick(grid->viewport(), Qt::LeftButton, Qt::NoModifier,
                          grid->visualItemRect(grid->item(0)).center());
        QVERIFY(selected.count() >= 1);
        const auto old = qgetenv("KETPLUS_KETATLAS_CLI");
        qputenv("KETPLUS_KETATLAS_CLI", "/missing/ketatlas");
        tab.openAtlas(atlas);
        tab.setDeferInterval(30);
        tab.setActive(false);
        QTRY_VERIFY(tab.isDeferred());
        QCOMPARE(tab.atlasPath(), atlas.path);
        tab.setActive(true);
        QVERIFY(!tab.isDeferred());
        QCOMPARE(tab.atlasPath(), atlas.path);
        if (old.isNull())
            qunsetenv("KETPLUS_KETATLAS_CLI");
        else
            qputenv("KETPLUS_KETATLAS_CLI", old);
    }
    void thumbnailWorkIsBounded() {
        ketplus::AtlasThumbnailCache cache;
        for (int i = 0; i < 80; ++i) {
            ketplus::AtlasDescriptor descriptor;
            descriptor.path = QStringLiteral("/synthetic/%1/atlas.json").arg(i);
            descriptor.title = QStringLiteral("Atlas %1").arg(i);
            cache.request(descriptor);
        }
        QVERIFY(cache.pendingCount() <= 32);
        QTRY_VERIFY_WITH_TIMEOUT(cache.pendingCount() == 0, 5000);
        QVERIFY(cache.cachedBytes() > 0);
        QVERIFY(cache.cachedBytes() <= 64 * 1024 * 1024);
    }
    void missingBundleKeepsInvalidThenOffersRecovery() {
        QTemporaryDir root;
        const auto atlas = fixture(root.path());
        const auto oldCli = qgetenv("KETPLUS_KETATLAS_CLI");
        qputenv("KETPLUS_KETATLAS_CLI", "/missing/ketatlas");
        ketplus::AtlasIndex index;
        index.setRoot(root.path());
        QTRY_COMPARE(index.atlases().size(), 1);
        ketplus::AtlasTab tab(&index);
        tab.resize(800, 600);
        tab.show();
        tab.openAtlas(atlas);
        auto* status = tab.findChild<QLabel*>("atlasStatus");
        auto* choose = tab.findChild<QAbstractButton*>("atlasChooseAnotherButton");
        auto* notice = tab.findChild<QLabel*>("atlasLastValidNotice");
        QVERIFY(status && choose && notice);
        write(atlas.path, "{\"version\":1");
        index.scheduleRefresh(0);
        QTRY_COMPARE(index.atlases().size(), 0);
        QTRY_VERIFY(notice->isVisible());
        QTest::qWait(1700);
        QVERIFY(choose->isHidden()); // Invalid candidate retains the last valid state.
        QVERIFY(QFile::remove(atlas.path));
        index.scheduleRefresh(0);
        QTRY_VERIFY_WITH_TIMEOUT(choose->isVisible(), 4000);
        QVERIFY(status->text().contains("no longer available"));
        choose->click();
        QVERIFY(choose->isHidden());
        QCOMPARE(tab.atlasPath(), QString{});
        if (oldCli.isNull())
            qunsetenv("KETPLUS_KETATLAS_CLI");
        else
            qputenv("KETPLUS_KETATLAS_CLI", oldCli);
    }
    void missingCliOffersExplicitInstall() {
#ifdef Q_OS_WIN
        QSKIP("Fake npm requires a POSIX shell");
#else
        QTemporaryDir root;
        QTemporaryDir tools;
        const auto atlas = fixture(root.path());
        const auto oldCli = qgetenv("KETPLUS_KETATLAS_CLI");
        const auto oldPath = qgetenv("PATH");
        qputenv("KETPLUS_KETATLAS_CLI", tools.filePath("missing-ketatlas").toUtf8());

        QFile npm(tools.filePath("npm"));
        QVERIFY(npm.open(QIODevice::WriteOnly));
        npm.write("#!/bin/sh\necho simulated install failure\nexit 7\n");
        npm.close();
        QVERIFY(npm.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", tools.path().toUtf8() + ':' + oldPath);

        ketplus::AtlasIndex index;
        index.setRoot(root.path());
        QTRY_VERIFY(!index.manifests().isEmpty());
        ketplus::AtlasTab tab(&index);
        tab.resize(800, 600);
        tab.show();
        tab.openAtlas(atlas);

        auto* status = tab.findChild<QLabel*>("atlasStatus");
        auto* command = tab.findChild<QLabel*>("atlasInstallCommand");
        auto* install = tab.findChild<QAbstractButton*>("atlasInstallButton");
        auto* locate = tab.findChild<QAbstractButton*>("atlasLocateCliButton");
        QVERIFY(status && command && install && locate);
        QVERIFY(status->text().contains("required to preview"));
        QCOMPARE(command->text(), QStringLiteral("npm install -g ketatlas@0.6.1"));
        QVERIFY(command->isVisible());
        QVERIFY(install->isVisible());
        QVERIFY(locate->isVisible());
        QVERIFY(!tab.findChild<QProcess*>("ketAtlasInstaller"));

        install->click();
        QTRY_VERIFY(tab.findChild<QProcess*>("ketAtlasInstaller"));
        QTRY_VERIFY(status->text().contains("exit 7"));
        QVERIFY(install->isEnabled());

        qputenv("PATH", oldPath);
        if (oldCli.isNull())
            qunsetenv("KETPLUS_KETATLAS_CLI");
        else
            qputenv("KETPLUS_KETATLAS_CLI", oldCli);
#endif
    }
    void serverLifecycle() {
        const auto cli = QStandardPaths::findExecutable(QStringLiteral("ketatlas"));
        if (cli.isEmpty())
            QSKIP("KetAtlas CLI is not installed");
        const auto oldCli = qgetenv("KETPLUS_KETATLAS_CLI");
        qputenv("KETPLUS_KETATLAS_CLI", cli.toUtf8());
        QTemporaryDir root;
        const auto atlas = fixture(root.path());
        ketplus::AtlasProcess process;
        QSignalSpy ready(&process, &ketplus::AtlasProcess::ready);
        QSignalSpy failed(&process, &ketplus::AtlasProcess::failed);
        process.start(atlas);
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 25000);
        QVERIFY2(!ready.isEmpty(),
                 failed.isEmpty() ? "No response" : qPrintable(failed.first().first().toString()));
        QNetworkAccessManager network;
        auto* reply = network.get(QNetworkRequest(ready.first().first().toUrl()));
        QTRY_VERIFY(reply->isFinished());
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QVERIFY(reply->readAll().contains("loadAtlas"));
        reply->deleteLater();
        process.stop();
        QVERIFY(!process.running());
        QCOMPARE(failed.count(), 0);
        if (oldCli.isNull())
            qunsetenv("KETPLUS_KETATLAS_CLI");
        else
            qputenv("KETPLUS_KETATLAS_CLI", oldCli);
    }
    void nativeAtlasSmoke() {
        if (!qEnvironmentVariableIsSet("KETPLUS_NATIVE_ATLAS_TEST"))
            QSKIP("Enable native WebView test explicitly");
        QTemporaryDir root;
        const auto atlas = fixture(root.path());
        ketplus::AtlasIndex index;
        ketplus::AtlasTab tab(&index);
        tab.resize(1000, 750);
        tab.show();
        QSignalSpy loaded(&tab, &ketplus::AtlasTab::loaded);
        tab.openAtlas(atlas);
        QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 30000);
        QWebView* browser = nullptr;
        for (auto* window : QGuiApplication::allWindows())
            if (auto* web = qobject_cast<QWebView*>(window))
                browser = web;
        QVERIFY(browser);
        QVariant state;
        browser->runJavaScript("JSON.stringify(window.atlas.getState())",
                               [&state](const QVariant& value) { state = value; });
        QTRY_VERIFY(state.isValid());
        QCOMPARE(QJsonDocument::fromJson(state.toString().toUtf8())
                     .object()
                     .value("screenCount")
                     .toInt(),
                 1);
        QTest::qWait(500);
        const auto capture = qEnvironmentVariable("KETPLUS_ATLAS_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(tab.screen()->grabWindow(tab.winId()).save(capture));
        tab.setDeferInterval(50);
        tab.setActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(tab.isDeferred(), 5000);
        tab.setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 30000);
    }
    void indexConfiguredWorkspace() {
        const auto root = qEnvironmentVariable("KETPLUS_INDEX_ROOT");
        if (root.isEmpty())
            QSKIP("Set KETPLUS_INDEX_ROOT for a real-workspace scan");
        QElapsedTimer timer;
        timer.start();
        const auto atlases = ketplus::AtlasIndex::scan(root);
        qInfo() << atlases.size() << "atlas manifests," << timer.elapsed() << "ms";
    }
};

int main(int argc, char** argv) {
    QtWebView::initialize();
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("KetPlusTests");
    QCoreApplication::setApplicationName("AtlasTest");
    AtlasTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "AtlasTest.moc"
