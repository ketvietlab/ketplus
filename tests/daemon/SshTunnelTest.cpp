#include "ssh/SshTunnel.h"
#include "ssh/SshHostKeys.h"
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

// SshTunnel against a loopback sshd on 127.0.0.1 with throwaway host and user keys; a
// QLocalServer plays the daemon. No real host, account, agent or ~/.ssh file is used.
using namespace ketplus::remote::ssh;

namespace {
QByteArray
runTool(const QString& program, const QStringList& arguments,
        const QProcessEnvironment& environment = QProcessEnvironment::systemEnvironment()) {
    QProcess process;
    process.setProcessEnvironment(environment);
    process.start(program, arguments);
    if (!process.waitForFinished(20000) || process.exitCode() != 0)
        return {};
    return process.readAllStandardOutput();
}
bool keygen(const QString& path) {
    QFile::remove(path);
    QFile::remove(path + ".pub");
    QProcess process;
    process.start("ssh-keygen", {"-q", "-t", "ed25519", "-N", "", "-C", "fixture", "-f", path});
    return process.waitForFinished(20000) && process.exitCode() == 0;
}
HostKey publicKey(const QString& path, const QString& name) {
    QFile file(path + ".pub");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const auto fields = QString::fromUtf8(file.readAll()).split(' ', Qt::SkipEmptyParts);
    return fields.size() >= 2 ? HostKey{name, fields.at(0), fields.at(1)} : HostKey{};
}
bool writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(contents) == contents.size();
}

// One sshd, one daemon socket and the ssh client config that reaches them.
struct SshFixture {
    QTemporaryDir dir{QDir::tempPath() + "/kp-ssh-XXXXXX"};
    QProcess sshd;
    QLocalServer daemon;
    QList<QLocalSocket*> daemonConnections;
    int port{0};
    QString user;
    HostKey hostKey;
    QString skip;

    QString path(const QString& name) const { return dir.filePath(name); }
    QString knownName() const { return QStringLiteral("[127.0.0.1]:%1").arg(port); }

    bool start(bool authorized = true, bool listening = true) {
        user = qEnvironmentVariable("USER", qEnvironmentVariable("LOGNAME"));
        if (!dir.isValid() || user.isEmpty() || !QFile::exists("/usr/sbin/sshd")) {
            skip = "sshd is unavailable";
            return false;
        }
        if (!keygen(path("host")) || !keygen(path("user")) || !keygen(path("other"))) {
            skip = "ssh-keygen is unavailable";
            return false;
        }
        hostKey = publicKey(path("host"), {});
        QFile pub(path(authorized ? "user.pub" : "other.pub"));
        if (!pub.open(QIODevice::ReadOnly) || !writeFile(path("authorized_keys"), pub.readAll())) {
            skip = "unable to write the fixture keys";
            return false;
        }
        QTcpServer probe;
        probe.listen(QHostAddress::LocalHost, 0);
        port = probe.serverPort();
        probe.close();
        hostKey.knownHostsName = knownName();
        writeFile(path("sshd_config"),
                  QStringLiteral("ListenAddress 127.0.0.1:%1\nHostKey %2\nPidFile %3\n"
                                 "AuthorizedKeysFile %4\nStrictModes no\nUsePAM no\n"
                                 "PasswordAuthentication no\nKbdInteractiveAuthentication no\n"
                                 "PubkeyAuthentication yes\nAllowUsers %5\n"
                                 "SetEnv KETPLUS_DAEMON_SOCKET=%6\nLogLevel ERROR\n")
                      .arg(port)
                      .arg(path("host"), path("sshd.pid"), path("authorized_keys"), user,
                           path("daemon.sock"))
                      .toUtf8());
        writeFile(path("ssh_config"),
                  QStringLiteral("Host fixture\n  HostName 127.0.0.1\n  Port %1\n  User %2\n"
                                 "  IdentityFile %3\n  IdentitiesOnly yes\n  IdentityAgent none\n"
                                 "  UserKnownHostsFile %4\n  GlobalKnownHostsFile /dev/null\n"
                                 "  ControlMaster no\n  ControlPath none\n  LogLevel ERROR\n")
                      .arg(port)
                      .arg(user, path("user"), path("user_known_hosts"))
                      .toUtf8());
        QFile::setPermissions(path("user"), QFile::ReadOwner | QFile::WriteOwner);
        writeFile(path("user_known_hosts"), {});
        if (listening) {
            QObject::connect(&daemon, &QLocalServer::newConnection, &daemon, [this] {
                while (auto* socket = daemon.nextPendingConnection()) {
                    daemonConnections.append(socket);
                    QObject::connect(socket, &QLocalSocket::readyRead, socket,
                                     [socket] { socket->write(socket->readAll()); });
                }
            });
            if (!daemon.listen(path("daemon.sock"))) {
                skip = "local socket unavailable";
                return false;
            }
        }
        sshd.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        sshd.start("/usr/sbin/sshd", {"-D", "-e", "-f", path("sshd_config")});
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (sshd.state() == QProcess::NotRunning)
                break;
            QTcpSocket socket;
            socket.connectToHost(QHostAddress::LocalHost, quint16(port));
            if (socket.waitForConnected(100))
                return true;
            QTest::qWait(50);
        }
        skip = "a non-root loopback sshd could not start here";
        return false;
    }
    ~SshFixture() {
        sshd.kill();
        sshd.waitForFinished(5000);
    }
    void trust(const HostKey& key) const {
        writeFile(path("user_known_hosts"),
                  QStringLiteral("%1 %2 %3\n").arg(knownName(), key.type, key.base64).toUtf8());
    }
    SshTunnel::Options options() const {
        SshTunnel::Options options;
        options.knownHostsFile = path("ketplus/known_hosts");
        options.sshArguments = {"-F", path("ssh_config")};
        options.stepTimeoutMilliseconds = 20000;
        options.initialBackoffMilliseconds = 100;
        options.maximumBackoffMilliseconds = 400;
        return options;
    }
};

constexpr int patience = 30000;
} // namespace

class SshTunnelTest : public QObject {
    Q_OBJECT

  private slots:
    void fingerprintsMatchSshKeygen() {
        QTemporaryDir dir;
        QVERIFY(keygen(dir.filePath("key")));
        const auto key = publicKey(dir.filePath("key"), "host");
        const auto listed =
            QString::fromUtf8(runTool("ssh-keygen", {"-l", "-f", dir.filePath("key.pub")}))
                .split(' ');
        QVERIFY(listed.size() > 1);
        QCOMPARE(key.fingerprint(), listed.at(1));
    }

    void parsesOpenSshOutputConservatively() {
        const auto keys = parseCaptured("NONE NONE NONE\n"
                                        "[h]:22 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIA==\n"
                                        "bad name ssh-ed25519 AAAA\n"
                                        "h ssh-ed25519 $(touch)\n");
        QCOMPARE(keys.size(), 1);
        QCOMPARE(keys.first().knownHostsName, QStringLiteral("[h]:22"));
        const auto known = parseKnownEntries("# Host h found: line 1\n"
                                             "h ssh-ed25519 AAAA\n"
                                             "@revoked h ssh-ed25519 BBBB\n"
                                             "@cert-authority * ssh-ed25519 CCCC\n"
                                             "|1|x|y ssh-rsa DDDD\n");
        QCOMPARE(known.value("ssh-ed25519"), QStringList{"AAAA"});
        QCOMPARE(known.value("ssh-rsa"), QStringList{"DDDD"});
        QCOMPARE(algorithmsFor({"ssh-rsa", "ssh-ed25519"}),
                 (QStringList{"rsa-sha2-512", "rsa-sha2-256", "ssh-ed25519"}));
        const auto files = knownHostsFiles(
            parseResolvedConfig("userknownhostsfile ~/.ssh/known_hosts ~/.ssh/known_hosts2\n"
                                "globalknownhostsfile /etc/ssh/ssh_known_hosts /dev/null\n"));
        QCOMPARE(files, (QStringList{QDir::homePath() + "/.ssh/known_hosts",
                                     QDir::homePath() + "/.ssh/known_hosts2",
                                     "/etc/ssh/ssh_known_hosts"}));
        QVERIFY(!(SshTarget{"-oProxyCommand=x", {}, 0}.isValid()));
        QVERIFY(!(SshTarget{"host", "a b", 0}.isValid()));
        QCOMPARE(SshTarget::fromJson(SshTarget{"box", "me", 2222}.toJson()).toJson(),
                 (QJsonObject{{"alias", "box"}, {"user", "me"}, {"port", 2222}}));
    }

    void socketProbeResolvesLikeTheUserUnit() {
        QTemporaryDir dir(QDir::tempPath() + "/kp-probe-XXXXXX");
        QDir().mkpath(dir.filePath("run"));
        QDir().mkpath(dir.filePath(".config/ketplus"));
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.remove("KETPLUS_DAEMON_SOCKET");
        environment.insert("HOME", dir.path());
        environment.insert("XDG_RUNTIME_DIR", dir.filePath("run"));
        const auto resolve = [&] {
            bool present = false;
            QString path;
            const bool parsed = parseSocketProbe(
                runTool("/bin/sh", {"-c", remoteSocketProbe()}, environment), &present, &path);
            return parsed ? QStringLiteral("%1 %2").arg(present ? "present" : "missing", path)
                          : QString();
        };
        QCOMPARE(resolve(), "missing " + dir.filePath("run/ketplus-daemon-v0"));
        QLocalServer server;
        QVERIFY(server.listen(dir.filePath("run/ketplus-daemon-v0")));
        QCOMPARE(resolve(), "present " + dir.filePath("run/ketplus-daemon-v0"));
        QVERIFY(writeFile(dir.filePath(".config/ketplus/daemon.env"),
                          "KETPLUS_DAEMON_SOCKET=\"/srv/kp/sock\"\n"));
        QCOMPARE(resolve(), QStringLiteral("missing /srv/kp/sock"));
        environment.insert("KETPLUS_DAEMON_SOCKET", "/env/sock");
        QCOMPARE(resolve(), QStringLiteral("missing /env/sock"));
        environment.insert("KETPLUS_DAEMON_SOCKET", "/env/a b");
        QVERIFY(resolve().isEmpty());
    }

    void anUnknownKeyAcceptedIsRecordedAndCarriesDaemonBytes() {
        SshFixture fixture;
        if (!fixture.start())
            QSKIP(qPrintable(fixture.skip));
        SshTunnel tunnel({"fixture", {}, 0}, fixture.options());
        QList<HostKey> asked;
        tunnel.setHostKeyPrompt([&](const HostKey& key, std::function<void(bool)> decide) {
            asked.append(key);
            QCOMPARE(fixture.daemonConnections.size(), 0);
            decide(true);
        });
        tunnel.open();
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::Connected, patience);
        QCOMPARE(asked.size(), 1);
        QCOMPARE(asked.first().fingerprint(), fixture.hostKey.fingerprint());
        QCOMPARE(tunnel.fingerprint(), fixture.hostKey.fingerprint());
        QCOMPARE(tunnel.hostKeyStatus(), HostKeyStatus::Verified);
        QCOMPARE(tunnel.remoteSocketPath(), fixture.path("daemon.sock"));
        QFile recorded(fixture.path("ketplus/known_hosts"));
        QVERIFY(recorded.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(recorded.readAll()),
                 QStringLiteral("%1 %2 %3\n")
                     .arg(fixture.knownName(), fixture.hostKey.type, fixture.hostKey.base64));
        QVERIFY(!(recorded.permissions() & (QFile::ReadGroup | QFile::ReadOther)));

        auto* device = tunnel.device();
        QVERIFY(device);
        device->write("ping");
        QTRY_VERIFY_WITH_TIMEOUT(device->bytesAvailable() >= 4, patience);
        QCOMPARE(device->readAll(), QByteArray("ping"));

        // The recorded key verifies the next connection without asking.
        SshTunnel again({"fixture", {}, 0}, fixture.options());
        again.setHostKeyPrompt([](const HostKey&, std::function<void(bool)>) { QFAIL("asked"); });
        again.open();
        QTRY_COMPARE_WITH_TIMEOUT(again.state(), SshTunnel::State::Connected, patience);
    }

    void anUnknownKeyRejectedNeverReachesTheDaemon() {
        SshFixture fixture;
        if (!fixture.start())
            QSKIP(qPrintable(fixture.skip));
        SshTunnel tunnel({"fixture", {}, 0}, fixture.options());
        tunnel.setHostKeyPrompt(
            [](const HostKey&, std::function<void(bool)> decide) { decide(false); });
        tunnel.open();
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::HostKeyRejected, patience);
        QTest::qWait(300);
        QCOMPARE(tunnel.state(), SshTunnel::State::HostKeyRejected);
        QVERIFY(!QFile::exists(fixture.path("ketplus/known_hosts")));
        QCOMPARE(fixture.daemonConnections.size(), 0);
        QVERIFY(!tunnel.device());
    }

    void aChangedHostKeyFailsClosedBeforeAnyDaemonByte() {
        SshFixture fixture;
        if (!fixture.start())
            QSKIP(qPrintable(fixture.skip));
        fixture.trust(publicKey(fixture.path("other"), {}));
        SshTunnel tunnel({"fixture", {}, 0}, fixture.options());
        bool asked = false;
        tunnel.setHostKeyPrompt([&](const HostKey&, std::function<void(bool)>) { asked = true; });
        tunnel.open();
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::HostKeyMismatch, patience);
        QCOMPARE(tunnel.hostKeyStatus(), HostKeyStatus::Mismatch);
        QCOMPARE(tunnel.fingerprint(), fixture.hostKey.fingerprint());
        QTest::qWait(300);
        QCOMPARE(tunnel.state(), SshTunnel::State::HostKeyMismatch);
        QCOMPARE(tunnel.reconnectAttempts(), 0);
        QVERIFY(!asked);
        QCOMPARE(fixture.daemonConnections.size(), 0);
    }

    void aRefusedAccountIsAuthFailed() {
        SshFixture fixture;
        if (!fixture.start(false))
            QSKIP(qPrintable(fixture.skip));
        fixture.trust(fixture.hostKey);
        SshTunnel tunnel({"fixture", {}, 0}, fixture.options());
        tunnel.open();
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::AuthFailed, patience);
        QCOMPARE(tunnel.hostKeyStatus(), HostKeyStatus::Verified);
        QCOMPARE(fixture.daemonConnections.size(), 0);
    }

    void aStoppedDaemonIsDaemonSocketMissing() {
        SshFixture fixture;
        if (!fixture.start(true, false))
            QSKIP(qPrintable(fixture.skip));
        fixture.trust(fixture.hostKey);
        SshTunnel tunnel({"fixture", {}, 0}, fixture.options());
        tunnel.open();
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::DaemonSocketMissing, patience);
        QCOMPARE(tunnel.remoteSocketPath(), fixture.path("daemon.sock"));
        // It keeps trying with bounded backoff, and connects once the daemon is up.
        QTRY_VERIFY_WITH_TIMEOUT(tunnel.reconnectAttempts() >= 1, patience);
        QObject::connect(&fixture.daemon, &QLocalServer::newConnection, &fixture.daemon, [&] {
            while (auto* socket = fixture.daemon.nextPendingConnection())
                fixture.daemonConnections.append(socket);
        });
        QVERIFY(fixture.daemon.listen(fixture.path("daemon.sock")));
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::Connected, patience);
    }

    void aDroppedConnectionReconnectsWithBackoff() {
        SshFixture fixture;
        if (!fixture.start())
            QSKIP(qPrintable(fixture.skip));
        fixture.trust(fixture.hostKey);
        SshTunnel tunnel({"fixture", {}, 0}, fixture.options());
        QList<SshTunnel::State> states;
        QObject::connect(&tunnel, &SshTunnel::stateChanged, &tunnel,
                         [&](SshTunnel::State state) { states.append(state); });
        tunnel.open();
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::Connected, patience);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.daemonConnections.size(), 1, patience);
        fixture.daemonConnections.first()->disconnectFromServer();
        QTRY_VERIFY_WITH_TIMEOUT(states.contains(SshTunnel::State::Offline), patience);
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::Connected, patience);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.daemonConnections.size(), 2, patience);
        QCOMPARE(tunnel.reconnectAttempts(), 0);
        tunnel.close();
        QCOMPARE(tunnel.state(), SshTunnel::State::Idle);
        QVERIFY(!tunnel.device());
    }
};

QTEST_GUILESS_MAIN(SshTunnelTest)
#include "SshTunnelTest.moc"
