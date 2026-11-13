#include "KetDialog.h"
#include "KetPlusProductVersion.h"
#include "SshFixture.h"
#include "remote/LinuxConnectDialog.h"
#include "remote/LinuxMachineModel.h"
#include "remote/LinuxSshMachines.h"
#include "ssh/SshDaemonInstaller.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTimer>
#include <openssl/evp.h>
#include <openssl/pem.h>

using namespace ketplus;
using namespace ketplus::remote::ssh;
using namespace ketplus::remote::ssh::testing;
namespace {
// Tiny loopback HTTP server for the real network downloader. The artifact is streamed from
// the task's complete cmake --install tree, not a copied executable or a fake daemon.
struct ReleaseServer : QTcpServer {
    QByteArray manifest;
    QByteArray signature;
    QString archive;
    int requests{0};
    EVP_PKEY* key{nullptr};
    DaemonReleaseChannel channel;
    explicit ReleaseServer(QObject* parent = nullptr) : QTcpServer(parent) {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    if (!buffer->contains("\r\n\r\n") || socket->property("answered").toBool())
                        return;
                    socket->setProperty("answered", true);
                    ++requests;
                    const auto path = buffer->split(' ').value(1);
                    if (path == "/manifest.json" || path == "/manifest.json.sig") {
                        const auto body = path.endsWith(".sig") ? signature : manifest;
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: " +
                                      QByteArray::number(body.size()) +
                                      "\r\nConnection: close\r\n\r\n" + body);
                        socket->disconnectFromHost();
                    } else if (path == "/runtime.tar.gz") {
                        auto* file = new QFile(archive, socket);
                        if (!file->open(QIODevice::ReadOnly)) {
                            socket->abort();
                            return;
                        }
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: " +
                                      QByteArray::number(file->size()) +
                                      "\r\nConnection: close\r\n\r\n");
                        const auto pump = [socket, file] {
                            while (!file->atEnd() && socket->bytesToWrite() < 256 * 1024)
                                socket->write(file->read(65536));
                            if (file->atEnd() && socket->bytesToWrite() == 0)
                                socket->disconnectFromHost();
                        };
                        connect(socket, &QTcpSocket::bytesWritten, socket, pump);
                        pump();
                    } else {
                        socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
                        socket->disconnectFromHost();
                    }
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    ~ReleaseServer() override { EVP_PKEY_free(key); }
    bool start() {
        const auto keyFile = qEnvironmentVariable("KETPLUS_T51_QA_KEY");
        QFile input(keyFile);
        if (!input.open(QIODevice::ReadOnly))
            return false;
        const auto pem = input.readAll();
        auto* bio = BIO_new_mem_buf(pem.constData(), int(pem.size()));
        key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!key || !listen(QHostAddress::LocalHost, 0))
            return false;
        QByteArray publicKey(32, Qt::Uninitialized);
        size_t size = 32;
        if (EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char*>(publicKey.data()),
                                        &size) != 1)
            return false;
        const auto origin = "http://127.0.0.1:" + QString::number(serverPort());
        channel = {QUrl(origin + "/manifest.json"), publicKey, KETPLUS_PRODUCT_VERSION,
                   "http://127.0.0.1:9", true};
        archive = qEnvironmentVariable("KETPLUS_T51_QA_ARCHIVE");
        QFile file(archive);
        QFile infoFile(qEnvironmentVariable("KETPLUS_T51_QA_INFO"));
        if (!file.open(QIODevice::ReadOnly) || !infoFile.open(QIODevice::ReadOnly))
            return false;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file))
            return false;
        const auto info = QJsonDocument::fromJson(infoFile.readAll()).object();
        if (info.value("control_url") != channel.controlUrl ||
            info.value("product_version") != channel.version)
            return false;
        const QJsonObject descriptor{
            {"schema", 1},
            {"product", "ketplus-daemon"},
            {"version", channel.version},
            {"tag", "v" + channel.version},
            {"revision", info.value("build_revision")},
            {"control_url", channel.controlUrl},
            {"artifacts",
             QJsonArray{QJsonObject{{"os", "linux"},
                                    {"arch", "x86_64"},
                                    {"layout", "ketplus-runtime-v1"},
                                    {"bytes", file.size()},
                                    {"sha256", QString::fromLatin1(hash.result().toHex())},
                                    {"url", origin + "/runtime.tar.gz"}}}}};
        manifest = QJsonDocument(descriptor).toJson(QJsonDocument::Compact);
        auto* context = EVP_MD_CTX_new();
        signature.resize(64);
        size = 64;
        const bool ok =
            EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1 &&
            EVP_DigestSign(context, reinterpret_cast<unsigned char*>(signature.data()), &size,
                           reinterpret_cast<const unsigned char*>(manifest.constData()),
                           size_t(manifest.size())) == 1;
        EVP_MD_CTX_free(context);
        return ok;
    }
};

QAbstractButton* button(QWidget* dialog, const char* name) {
    return dialog->findChild<QAbstractButton*>(QString::fromLatin1(name));
}
void isolate(SshFixture& fixture) {
    // sshd overrides HOME/XDG for this QA connection only; no OS user, sudo, real account
    // directory or shared daemon is touched. Every child uses a loopback-only runtime.
    fixture.extraServerConfiguration =
        QStringLiteral("SetEnv HOME=%1 XDG_CONFIG_HOME=%1/config XDG_DATA_HOME=%1/data "
                       "XDG_RUNTIME_DIR=%1/runtime "
                       "KETPLUS_DAEMON_DATA_DIR=%1/data/Ketsuite/KetPlus\n")
            .arg(fixture.dir.path());
}
} // namespace

class SshInstallUiTest final : public QObject {
    Q_OBJECT
  private slots:
    void signedInstallDeclineConnectAndUninstallOnIsolatedProfile() {
        if (qEnvironmentVariableIsEmpty("KETPLUS_T51_QA_KEY"))
            QSKIP("Set the documented task-local QA key/archive/info to run the Linux-host proof.");
        ReleaseServer server;
        QVERIFY2(server.start(),
                 "The QA key, loopback-only full runtime or manifest fixture is unavailable.");
        SshFixture fixture;
        isolate(fixture);
        QVERIFY2(fixture.start(true, false), qPrintable(fixture.skip));
        fixture.trust(fixture.hostKey);
        QFile::remove(fixture.path("local.token"));
        const auto stopOwnedDaemon = qScopeGuard([&fixture] {
            if (!QFileInfo::exists(fixture.path("daemon.sock")))
                return;
            QProcess stop;
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert("HOME", fixture.dir.path());
            env.insert("XDG_DATA_HOME", fixture.path("data"));
            env.insert("KETPLUS_DAEMON_DATA_DIR", fixture.path("data/Ketsuite/KetPlus"));
            stop.setProcessEnvironment(env);
            stop.start(KETPLUS_TEST_CTL, {"--socket", fixture.path("daemon.sock"), "--token-file",
                                          fixture.path("local.token"), "--timeout", "5", "call",
                                          "daemon.stop_if_idle", "{}"});
            stop.waitForFinished(10000);
        });
        const auto root = fixture.path(".local/opt/ketplus");
        const auto options = fixture.options();
        LinuxMachineModel model;
        LinuxSshMachines machines(
            &model, fixture.path("machines.json"),
            [options](const SshTarget& target) -> std::unique_ptr<SshLink> {
                return std::make_unique<SshTunnelLink>(target, options);
            },
            nullptr, server.channel);
        QWidget anchor;
        anchor.resize(900, 900);
        anchor.show();
        QSignalSpy offered(&model, &LinuxMachineModel::daemonInstallOffered);
        QSignalSpy consent(&model, &LinuxMachineModel::daemonConsentRequested);
        QSignalSpy failed(&model, &LinuxMachineModel::sshFailed);
        QSignalSpy connected(&model, &LinuxMachineModel::sshConnected);
        const auto open = [&](bool manage = false) {
            auto* dialog = openLinuxConnectDialog(&anchor, &model, nullptr, {},
                                                  LinuxConnectMode::Ssh, {}, manage);
            auto* host = dialog->findChild<QLineEdit*>("linuxSshHost");
            host->setText("fixture");
            button(dialog, "linuxSshConnect")->click();
            return QPointer<ket::Dialog>(dialog);
        };
        auto decline = open();
        QTRY_COMPARE_WITH_TIMEOUT(offered.size(), 1, patience);
        QVERIFY(!QFileInfo::exists(root));
        button(decline, "linuxDaemonReviewInstall")->click();
        QTRY_COMPARE_WITH_TIMEOUT(consent.size(), 1, patience);
        QVERIFY(!QFileInfo::exists(root)); // Review performs no installation writes.
        button(decline, "linuxDaemonDecline")->click();
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, patience);
        QVERIFY(!QFileInfo::exists(root));
        QCOMPARE(server.requests, 2); // Manifest + signature, no artifact on declined consent.
        decline->reject();
        QTest::qWait(20);

        auto install = open();
        QTRY_COMPARE_WITH_TIMEOUT(offered.size(), 2, patience);
        button(install, "linuxDaemonReviewInstall")->click();
        QTRY_COMPARE_WITH_TIMEOUT(consent.size(), 2, patience);
        const auto evidence = qEnvironmentVariable("KETPLUS_T51_QA_EVIDENCE");
        if (!evidence.isEmpty()) {
            QDir().mkpath(evidence);
            QVERIFY(install->grab().save(QDir(evidence).filePath("install-consent.png")));
        }
        button(install, "linuxDaemonConfirm")->click();
        QTRY_COMPARE_WITH_TIMEOUT(connected.size(), 1, 120000);
        QVERIFY(QFileInfo::exists(root + "/ssh-install.json"));
        QVERIFY(QFileInfo(root + "/current").isSymLink());
        QCOMPARE(model.machines().size(), 1);
        QCOMPARE(model.machines().first().state, LinuxMachine::State::Connected);
        qInfo("QA proof: signed full runtime -> verified SSH -> authenticated daemon.hello -> "
              "Connected; same UID, isolated HOME/XDG, loopback-only control URL.");
        QVERIFY(fixture.unhijacked());

        auto uninstall = open(true);
        QTRY_COMPARE_WITH_TIMEOUT(offered.size(), 3, patience);
        button(uninstall, "linuxDaemonUninstall")->click();
        QTRY_COMPARE_WITH_TIMEOUT(consent.size(), 3, patience);
        button(uninstall, "linuxDaemonConfirm")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo(root + "/current").isSymLink(), patience);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(root + "/ssh-install.json"), patience);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(fixture.path("daemon.sock")), patience);
        QVERIFY(QFileInfo::exists(fixture.path("local.token"))); // User data/credential retained.
        qInfo("QA proof: idle-only authenticated uninstall removed managed runtime and activation; "
              "user data retained. No OS user, sudo, service or relay registration.");
        if (uninstall)
            uninstall->reject();
    }
};
QTEST_MAIN(SshInstallUiTest)
#include "SshInstallUiTest.moc"
