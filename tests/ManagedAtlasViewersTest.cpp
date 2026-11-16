#include "atlas/ManagedAtlasViewers.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "atlas/AtlasRendererPermission.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

class ManagedAtlasViewersTest final : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> root_;
    QByteArray oldCli_, oldRecord_;
    static bool write(const QString& path, const QByteArray& bytes) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    }
    static QByteArray read(const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    }
    static QByteArray contract(const QString& cwd = ".") {
        return QJsonDocument(
                   QJsonObject{
                       {"version", 1},
                       {"framework", "fixture"},
                       {"cwd", cwd},
                       {"command", QJsonArray{QCoreApplication::applicationFilePath(),
                                              "<b>local only</b>", "{port}", "{atlasDirectory}"}}})
            .toJson();
    }
    ketplus::AtlasDescriptor fixture() {
        const auto path = root_->filePath("sample.ketatlas/atlas.json");
        write(path, R"({"version":1,"title":"Local fixture","screens":[],"flows":[]})");
        write(root_->filePath("sample.ketatlas/atlas.renderer.json"), contract());
        return ketplus::AtlasIndex::readManifest(path, root_->path());
    }
    static ket::Dialog* pending(QWidget& window) {
        for (auto* dialog : window.findChildren<ket::Dialog*>("atlasRendererPermission"))
            if (dialog->isOpen())
                return dialog;
        return nullptr;
    }
    static void restore(const char* name, const QByteArray& bytes) {
        bytes.isNull() ? qunsetenv(name) : qputenv(name, bytes);
    }

  public slots:
    void openUrl(const QUrl&) {} // Never invoke a real browser from these fixtures.

  private slots:
    void initTestCase() { QDesktopServices::setUrlHandler("http", this, "openUrl"); }
    void cleanupTestCase() { QDesktopServices::unsetUrlHandler("http"); }
    void init() {
        root_ = std::make_unique<QTemporaryDir>();
        QVERIFY(root_->isValid());
        oldCli_ = qgetenv("KETPLUS_KETATLAS_CLI");
        oldRecord_ = qgetenv("KETPLUS_ATLAS_FIXTURE_RECORD");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root_->path());
        QFile source(QFINDTESTDATA("fixtures/fake-atlas-managed.sh"));
        QVERIFY(source.open(QIODevice::ReadOnly));
        const auto cli = root_->filePath("fake-atlas");
        QVERIFY(write(cli, source.readAll()));
        QVERIFY(QFile::setPermissions(cli, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                               QFileDevice::ExeOwner));
        qputenv("KETPLUS_KETATLAS_CLI", cli.toUtf8());
        qputenv("KETPLUS_ATLAS_FIXTURE_RECORD", root_->path().toUtf8());
    }
    void cleanup() {
        restore("KETPLUS_KETATLAS_CLI", oldCli_);
        restore("KETPLUS_ATLAS_FIXTURE_RECORD", oldRecord_);
        root_.reset();
    }
    void consent_data() {
        QTest::addColumn<QString>("action");
        for (const auto* action :
             {"run", "return-run", "return-static", "refuse", "escape", "close", "contract-change",
              "manifest-change", "cwd-change", "sidecar-link", "cwd-link", "bundle-link", "stop",
              "no-anchor", "invalid", "static"})
            QTest::newRow(action) << QString::fromLatin1(action);
    }
    void consent() {
#ifndef Q_OS_UNIX
        QSKIP("The fake CLI fixture uses a Unix shell.");
#else
        QFETCH(QString, action);
        const auto atlas = fixture();
        QVERIFY(!atlas.path.isEmpty());
        const auto sidecar = atlas.directory + "/atlas.renderer.json";
        if (action == "static")
            QVERIFY(QFile::remove(sidecar));
        if (action == "invalid")
            QVERIFY(write(sidecar, "not json"));
        if (action == "cwd-link") {
            QVERIFY(QDir().mkpath(atlas.directory + "/work"));
            QVERIFY(write(sidecar, contract("work")));
        }
        QWidget window;
        window.resize(1000, 720);
        window.show();
        ketplus::ManagedAtlasViewers viewers;
        viewers.open(atlas, action == "no-anchor" ? nullptr : &window);
        auto* dialog = pending(window);
        const bool prompt = action != "static" && action != "invalid" && action != "no-anchor";
        QCOMPARE(dialog != nullptr, prompt);
        if (dialog) {
            QVERIFY(!QFileInfo::exists(root_->filePath("argv")));
            auto* label = dialog->findChild<QLabel*>("atlasRendererCommand");
            QVERIFY(label);
            QCOMPARE(label->textFormat(), Qt::PlainText);
            QVERIFY(label->text().contains("<b>local only</b>"));
            QVERIFY(label->text().contains(atlas.directory));
            QVERIFY(!label->text().contains("{port}"));
            viewers.open(atlas, &window);
            QCOMPARE(window.findChildren<ket::Dialog*>("atlasRendererPermission").size(), 1);
            if (action == "contract-change")
                QVERIFY(write(sidecar, contract() + "\n"));
            if (action == "manifest-change")
                QVERIFY(write(atlas.path, read(atlas.path) + "\n"));
            if (action == "cwd-change") {
                QVERIFY(QDir().mkpath(atlas.directory + "/new-cwd"));
                QVERIFY(write(sidecar, contract("new-cwd")));
            }
            if (action == "sidecar-link") {
                const auto outside = root_->filePath("other/atlas.renderer.json");
                QVERIFY(write(outside, contract()));
                QVERIFY(QFile::remove(sidecar));
                QCOMPARE(::symlink(QFile::encodeName(outside), QFile::encodeName(sidecar)), 0);
            }
            if (action == "cwd-link") {
                QVERIFY(QDir(atlas.directory).rename("work", "old-work"));
                QVERIFY(QDir().mkpath(atlas.directory + "/other-work"));
                QCOMPARE(::symlink("other-work", QFile::encodeName(atlas.directory + "/work")), 0);
            }
            if (action == "bundle-link") {
                QVERIFY(QDir(root_->path()).rename("sample.ketatlas", "moved.ketatlas"));
                QCOMPARE(::symlink("moved.ketatlas", QFile::encodeName(atlas.directory)), 0);
            }
            if (action == "stop") {
                viewers.stop(atlas.path);
                QTest::qWait(50);
                QVERIFY(!QFileInfo::exists(root_->filePath("argv")));
                QVERIFY(viewers.viewers().isEmpty());
                return;
            }
            if (action == "escape")
                QTest::keyClick(dialog, Qt::Key_Escape);
            else if (action.startsWith("return-")) {
                auto* button = dialog->findChild<ket::Button*>(
                    action == "return-run" ? "atlasRendererRun" : "atlasRendererStatic");
                window.activateWindow();
                QCoreApplication::processEvents();
                button->setFocus();
                QVERIFY(button->hasFocus());
                QTest::keyClick(button, Qt::Key_Return);
            } else if (action == "close")
                dialog->reject();
            else
                dialog
                    ->findChild<ket::Button*>(action == "refuse" ? "atlasRendererStatic"
                                                                 : "atlasRendererRun")
                    ->click();
        }
        if (action == "bundle-link") {
            QVERIFY(viewers.viewers().isEmpty());
            QVERIFY(!viewers.errorFor(atlas.path).isEmpty());
            QVERIFY(!QFileInfo::exists(root_->filePath("argv")));
            return;
        }
        QTRY_COMPARE(viewers.runningCount(), 1);
        const auto arguments = read(root_->filePath("argv"));
        const bool authorized = action == "run" || action == "return-run";
        QCOMPARE(arguments.contains("--renderer\n"), authorized);
        QCOMPARE(QFileInfo::exists(root_->filePath("renderer")), authorized);
        QCOMPARE(read(root_->filePath("cwd")).trimmed(), atlas.directory.toUtf8());
        QVERIFY(arguments.contains("--no-preload"));
        if (action.endsWith("change") || action.endsWith("link") || action == "invalid")
            QVERIFY(!viewers.errorFor(atlas.path).isEmpty());
        viewers.stop(atlas.path);
        QTRY_VERIFY(viewers.viewers().isEmpty());
#endif
    }
    void reopenAndRestartRequireFreshConsent() {
#ifndef Q_OS_UNIX
        QSKIP("The fake CLI fixture uses a Unix shell.");
#else
        const auto atlas = fixture();
        QWidget window;
        window.resize(1000, 720);
        window.show();
        for (int restart = 0; restart < 2; ++restart) {
            ketplus::ManagedAtlasViewers viewers;
            for (int reopen = 0; reopen < 2; ++reopen) {
                viewers.open(atlas, &window);
                auto* dialog = pending(window);
                QVERIFY(dialog);
                dialog->findChild<ket::Button*>("atlasRendererRun")->click();
                QTRY_COMPARE(viewers.runningCount(), 1);
                viewers.stop(atlas.path);
                QTRY_VERIFY(viewers.viewers().isEmpty());
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
        }
#endif
    }
    void commandDetailsEscapeControls() {
        const auto atlas = fixture();
        auto object = QJsonDocument::fromJson(contract()).object();
        object.insert("command", QJsonArray{QCoreApplication::applicationFilePath(),
                                            QStringLiteral("one\ntwo") + QChar(0x202e) + "<tag>"});
        QVERIFY(write(atlas.directory + "/atlas.renderer.json", QJsonDocument(object).toJson()));
        const auto permission = ketplus::AtlasRendererPermission::read(
            atlas, QProcessEnvironment::systemEnvironment(), 43121);
        QVERIFY(permission.error.isEmpty());
        QWidget window;
        window.resize(1000, 720);
        auto* dialog = ketplus::rendererPermissionDialog(&window, permission);
        const auto text = dialog->findChild<QLabel*>("atlasRendererCommand")->text();
        QVERIFY(!text.contains(QChar(0x202e)));
        QVERIFY(!text.contains(QStringLiteral("one\ntwo")));
        QVERIFY(text.contains("u000a"));
        QVERIFY(text.contains("u202e"));
        QVERIFY(text.contains("<tag>"));
        delete dialog;
    }
    void liveThemeZoomAndLayout() {
        ketplus::ThemeManager theme(*qApp);
        QWidget window;
        if (QGuiApplication::platformName() == "cocoa") {
            const auto screens = QGuiApplication::screens();
            if (screens.size() < 2)
                QSKIP("Native capture requires the secondary work display.");
            window.setScreen(screens.at(1));
            window.move(screens.at(1)->availableGeometry().topLeft() + QPoint(24, 24));
            qInfo() << "Native permission UI" << screens.at(1)->name() << "DPR"
                    << screens.at(1)->devicePixelRatio();
        }
        window.resize(1000, 720);
        window.show();
        const auto atlas = fixture();
        const auto permission = ketplus::AtlasRendererPermission::read(
            atlas, QProcessEnvironment::systemEnvironment(), 43121);
        QVERIFY(permission.error.isEmpty());
        auto* dialog = ketplus::rendererPermissionDialog(&window, permission);
        for (const auto mode :
             {ketplus::ThemeManager::Mode::Light, ketplus::ThemeManager::Mode::Dark,
              ketplus::ThemeManager::Mode::System}) {
            theme.setMode(mode);
            for (const int zoom : {90, 100, 110, 125}) {
                theme.setInterfaceScale(13, zoom);
                for (const auto size : {QSize(1000, 720), QSize(640, 600)}) {
                    window.resize(size);
                    QCoreApplication::processEvents();
                    QVERIFY(window.rect().contains(dialog->card()->geometry()));
                    for (auto* button : dialog->findChildren<ket::Button*>()) {
                        const auto position = button->mapTo(dialog->card(), QPoint{});
                        QVERIFY(dialog->card()->rect().contains(QRect(position, button->size())));
                        QVERIFY(button->isVisible());
                    }
                }
            }
        }
        if (const auto dir = qEnvironmentVariable("KETPLUS_ATLAS_PERMISSION_CAPTURE_DIR");
            !dir.isEmpty()) {
            QVERIFY(QDir().mkpath(dir));
            for (const auto mode :
                 {ketplus::ThemeManager::Mode::Light, ketplus::ThemeManager::Mode::Dark}) {
                theme.setMode(mode);
                QCoreApplication::processEvents();
                QVERIFY(window.grab().save(dir + (mode == ketplus::ThemeManager::Mode::Light
                                                      ? "/light.png"
                                                      : "/dark.png")));
            }
        }
        dialog->reject();
        delete dialog;
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("KetPlusTests");
    QCoreApplication::setApplicationName("ManagedAtlasViewersTest");
    ManagedAtlasViewersTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ManagedAtlasViewersTest.moc"
