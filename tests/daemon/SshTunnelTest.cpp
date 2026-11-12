#include "ssh/SshTunnel.h"
#include "ssh/SshHostKeys.h"
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>
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
bool keygen(const QString& path, const QString& type = "ed25519") {
    QFile::remove(path);
    QFile::remove(path + ".pub");
    QProcess process;
    process.start("ssh-keygen", {"-q", "-t", type, "-N", "", "-C", "fixture", "-f", path});
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

// Every message logged while a test runs.
QStringList& logged() {
    static QStringList messages;
    return messages;
}
void capture(QtMsgType, const QMessageLogContext&, const QString& message) {
    logged().append(message);
}

// One sshd, one daemon socket and the ssh client config that reaches them. The client config
// also carries the options a user's own config may hold that would hijack a call: a shared
// ControlMaster, a RemoteCommand, a LocalForward and a LocalCommand.
struct SshFixture {
    QTemporaryDir dir{QDir::tempPath() + "/kp-ssh-XXXXXX"};
    QProcess sshd;
    QLocalServer daemon;
    QList<QLocalSocket*> daemonConnections;
    int port{0};
    QString user;
    HostKey hostKey;
    HostKey ecdsaKey;
    QByteArray token{"fixture-local-token\n"};
    QString skip;

    QString path(const QString& name) const { return dir.filePath(name); }
    QString knownName() const { return QStringLiteral("[127.0.0.1]:%1").arg(port); }

    bool start(bool authorized = true, bool listening = true) {
        user = qEnvironmentVariable("USER", qEnvironmentVariable("LOGNAME"));
        if (!dir.isValid() || user.isEmpty() || !QFile::exists("/usr/sbin/sshd")) {
            skip = "sshd is unavailable";
            return false;
        }
        if (!keygen(path("host")) || !keygen(path("host_ecdsa"), "ecdsa") ||
            !keygen(path("user")) || !keygen(path("other")) ||
            !keygen(path("other_ecdsa"), "ecdsa") || !keygen(path("other_rsa"), "rsa")) {
            skip = "ssh-keygen is unavailable";
            return false;
        }
        hostKey = publicKey(path("host"), {});
        ecdsaKey = publicKey(path("host_ecdsa"), {});
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
        ecdsaKey.knownHostsName = knownName();
        writeFile(path("local.token"), token);
        QFile::setPermissions(path("local.token"), QFile::ReadOwner | QFile::WriteOwner);
        writeFile(path("sshd_config"),
                  QStringLiteral("ListenAddress 127.0.0.1:%1\nHostKey %2\nHostKey %2_ecdsa\n"
                                 "PidFile %3\n"
                                 "AuthorizedKeysFile %4\nStrictModes no\nUsePAM no\n"
                                 "PasswordAuthentication no\nKbdInteractiveAuthentication no\n"
                                 "PubkeyAuthentication yes\nAllowUsers %5\n"
                                 "SetEnv KETPLUS_DAEMON_SOCKET=%6 KETPLUS_DAEMON_TOKEN_FILE=%7\n"
                                 "LogLevel ERROR\nBanner %8\n")
                      .arg(port)
                      .arg(path("host"), path("sshd.pid"), path("authorized_keys"), user,
                           path("daemon.sock"), path("local.token"), path("banner"))
                      .toUtf8());
        writeFile(path("ssh_config"),
                  QStringLiteral("Host fixture\n  HostName 127.0.0.1\n  Port %1\n  User %2\n"
                                 "  IdentityFile %3\n  IdentitiesOnly yes\n  IdentityAgent none\n"
                                 "  UserKnownHostsFile %4\n  GlobalKnownHostsFile /dev/null\n"
                                 "  ControlMaster auto\n  ControlPath %5\n  ControlPersist 60\n"
                                 "  RemoteCommand echo hijacked\n  LocalForward %6 /tmp/nowhere\n"
                                 "  PermitLocalCommand yes\n  LocalCommand touch %7\n")
                      .arg(port)
                      .arg(user, path("user"), path("user_known_hosts"), path("cm-%r"),
                           path("forward.sock"), path("local-command"))
                      .toUtf8());
        QFile::setPermissions(path("user"), QFile::ReadOwner | QFile::WriteOwner);
        writeFile(path("user_known_hosts"), {});
        // A banner that imitates ssh's own refusals.
        writeFile(path("banner"), "fixture: Permission denied (publickey).\n"
                                  "Host key verification failed.\n");
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
    void trust(const HostKey& key, const QString& marker = {}) const {
        writeFile(path("user_known_hosts"), QStringLiteral("%1%2 %3 %4\n")
                                                .arg(marker.isEmpty() ? QString() : marker + ' ',
                                                     knownName(), key.type, key.base64)
                                                .toUtf8());
    }
    // Nothing from the user's config ran: no master, no forward, no local command.
    bool unhijacked() const {
        return QDir(dir.path()).entryList({"cm-*"}, QDir::AllEntries | QDir::System).isEmpty() &&
               !QFile::exists(path("forward.sock")) && !QFile::exists(path("local-command"));
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
        QCOMPARE(known.keys.value("ssh-ed25519"), QStringList{"AAAA"});
        QCOMPARE(known.keys.value("ssh-rsa"), QStringList{"DDDD"});
        QCOMPARE(known.revoked, QStringList{"BBBB"});
        QVERIFY(known.certificateAuthority);
        QVERIFY(parseKnownEntries("# none\n").isEmpty());

        // Only whole lines in ssh's own wording count; a bind error or banner text does not.
        QCOMPARE(classifySshError("me@h: Permission denied (publickey,password).\n"),
                 SshFailure::AuthFailed);
        QCOMPARE(classifySshError("unix_listener: cannot bind to path /x: Permission denied\n"
                                  "Welcome! Permission denied (publickey). Host key verification "
                                  "failed.\n"),
                 SshFailure::Other);
        QCOMPARE(classifySshError("@@@@\n@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     "
                                  "@\n"),
                 SshFailure::HostKeyChanged);
        QCOMPARE(classifySshError("Host key verification failed.\r\n"), SshFailure::HostKeyChanged);
        QCOMPARE(classifySshError("Unable to negotiate with 127.0.0.1 port 22: no matching host "
                                  "key type found. Their offer: ssh-ed25519\n"),
                 SshFailure::NoMatchingHostKeyType);
        QCOMPARE(classifySshError("ssh: connect to host h port 22: Connection timed out\n"),
                 SshFailure::Other);
        QCOMPARE(algorithmsFor({"ssh-rsa", "ssh-ed25519"}),
                 (QStringList{"rsa-sha2-512", "rsa-sha2-256", "ssh-ed25519"}));
        const auto files = knownHostsFiles(
            parseResolvedConfig("userknownhostsfile ~/.ssh/known_hosts ~/.ssh/known_hosts2\n"
                                "globalknownhostsfile /etc/ssh/ssh_known_hosts /dev/null\n"));
        QCOMPARE(files, (QStringList{QDir::homePath() + "/.ssh/known_hosts",
                                     QDir::homePath() + "/.ssh/known_hosts2",
                                     "/etc/ssh/ssh_known_hosts"}));
        // ssh -G prints paths unquoted: a path with a space is rejoined when it exists.
        QTemporaryDir spaced;
        QVERIFY(QDir().mkpath(spaced.filePath("my keys")));
        QVERIFY(writeFile(spaced.filePath("my keys/known hosts"), {}));
        const auto joined = spaced.filePath("my keys/known hosts");
        QCOMPARE(knownHostsFiles(parseResolvedConfig(
                     ("userknownhostsfile " + joined + " /tmp/second\n").toUtf8())),
                 (QStringList{joined, "/tmp/second"}));
        QCOMPARE(knownHostsFiles(parseResolvedConfig("userknownhostsfile \"/a b/c\" /d\n")),
                 (QStringList{"/a b/c", "/d"}));
        QVERIFY(!(SshTarget{"-oProxyCommand=x", {}, 0}.isValid()));
        QVERIFY(!(SshTarget{"host", "a b", 0}.isValid()));
        QCOMPARE(SshTarget::fromJson(SshTarget{"box", "me", 2222}.toJson()).toJson(),
                 (QJsonObject{{"alias", "box"}, {"user", "me"}, {"port", 2222}}));
    }

    void socketProbeResolvesLikeTheUserUnit() {
        QTemporaryDir dir(QDir::tempPath() + "/kp-probe-XXXXXX");
        QDir().mkpath(dir.filePath("run"));
        QDir().mkpath(dir.filePath(".config/ketplus"));
        QDir().mkpath(dir.filePath("data/daemon"));
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto* name : {"KETPLUS_DAEMON_SOCKET", "KETPLUS_DAEMON_TOKEN_FILE",
                                 "KETPLUS_DAEMON_DATA_DIR", "XDG_DATA_HOME"})
            environment.remove(QLatin1String(name));
        environment.insert("HOME", dir.path());
        environment.insert("XDG_RUNTIME_DIR", dir.filePath("run"));
        DaemonProbe probe;
        const auto resolve = [&] {
            probe = {};
            const bool parsed = parseDaemonProbe(
                runTool("/bin/sh", {"-c", remoteSocketProbe()}, environment), &probe);
            return parsed ? QStringLiteral("%1 %2").arg(probe.present ? "present" : "missing",
                                                        probe.socketPath)
                          : QString();
        };
        QCOMPARE(resolve(), "missing " + dir.filePath("run/ketplus-daemon-v0"));
        QLocalServer server;
        QVERIFY(server.listen(dir.filePath("run/ketplus-daemon-v0")));
        QCOMPARE(resolve(), "present " + dir.filePath("run/ketplus-daemon-v0"));
        // A socket without its token: not installed or not initialised.
        QCOMPARE(probe.token, DaemonProbe::Token::Missing);

        // The token: the environment, then daemon.env, then <data dir>/daemon/local.token.
        const auto owner = QFile::ReadOwner | QFile::WriteOwner;
        QVERIFY(writeFile(dir.filePath("data/daemon/local.token"), "default"));
        QVERIFY(QFile::setPermissions(dir.filePath("data/daemon/local.token"), owner));
        QVERIFY(writeFile(dir.filePath(".config/ketplus/daemon.env"),
                          ("KETPLUS_DAEMON_DATA_DIR=" + dir.filePath("data") + "\n").toUtf8()));
        resolve();
        QCOMPARE(probe.token, DaemonProbe::Token::Present);
        QCOMPARE(probe.credential, QByteArray("default"));
        QVERIFY(writeFile(dir.filePath("env.token"), "from-env-file"));
        QVERIFY(QFile::setPermissions(dir.filePath("env.token"), owner));
        QVERIFY(writeFile(
            dir.filePath(".config/ketplus/daemon.env"),
            ("KETPLUS_DAEMON_TOKEN_FILE=\"" + dir.filePath("env.token") + "\"\n").toUtf8()));
        resolve();
        QCOMPARE(probe.credential, QByteArray("from-env-file"));
        QVERIFY(writeFile(dir.filePath("variable.token"), QByteArray("\x01\xff"
                                                                     "binary\n",
                                                                     9)));
        QVERIFY(QFile::setPermissions(dir.filePath("variable.token"), owner));
        environment.insert("KETPLUS_DAEMON_TOKEN_FILE", dir.filePath("variable.token"));
        resolve();
        QCOMPARE(probe.credential, QByteArray("\x01\xff"
                                              "binary\n",
                                              9));
        // Readable by the group or others: refused, and its bytes are never printed.
        QVERIFY(QFile::setPermissions(dir.filePath("variable.token"), owner | QFile::ReadGroup));
        const auto output = runTool("/bin/sh", {"-c", remoteSocketProbe()}, environment);
        QVERIFY(!output.contains(QByteArray("\x01\xff"
                                            "binary",
                                            8)
                                     .toBase64()));
        QVERIFY(parseDaemonProbe(output, &probe));
        QCOMPARE(probe.token, DaemonProbe::Token::Permissive);
        QVERIFY(probe.credential.isEmpty());

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
        logged().clear();
        QLoggingCategory::setFilterRules("ketplus.remote.ssh*=true");
        const auto previous = qInstallMessageHandler(capture);
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
        QCOMPARE(tunnel.daemonCredential(), fixture.token);
        device->write("ping");
        QTRY_VERIFY_WITH_TIMEOUT(device->bytesAvailable() >= 4, patience);
        QCOMPARE(device->readAll(), QByteArray("ping"));
        QVERIFY(fixture.unhijacked());
        tunnel.close();
        QVERIFY(tunnel.daemonCredential().isEmpty());
        qInstallMessageHandler(previous);
        QVERIFY(!logged().isEmpty());
        for (const auto& message : std::as_const(logged())) {
            QVERIFY2(!message.contains("fixture-local-token"), qPrintable(message));
            QVERIFY(!message.contains(QString::fromLatin1(fixture.token.toBase64())));
        }

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
        QVERIFY(tunnel.daemonCredential().isEmpty());
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
        QCOMPARE(tunnel.daemonCredential(), fixture.token);
        QObject::connect(&tunnel, &SshTunnel::stateChanged, &tunnel, [&](SshTunnel::State state) {
            if (state != SshTunnel::State::Connected)
                QVERIFY(tunnel.daemonCredential().isEmpty());
        });
        fixture.daemonConnections.first()->disconnectFromServer();
        QTRY_VERIFY_WITH_TIMEOUT(states.contains(SshTunnel::State::Offline), patience);
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::Connected, patience);
        QCOMPARE(tunnel.daemonCredential(), fixture.token);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.daemonConnections.size(), 2, patience);
        QCOMPARE(tunnel.reconnectAttempts(), 0);
        tunnel.close();
        QCOMPARE(tunnel.state(), SshTunnel::State::Idle);
        QVERIFY(!tunnel.device());
        QVERIFY(tunnel.daemonCredential().isEmpty());
        QVERIFY(fixture.unhijacked());
    }

    void aHostKnownByAnotherKeyTypeIsVerifiedWithASecondProbe() {
        SshFixture fixture;
        if (!fixture.start())
            QSKIP(qPrintable(fixture.skip));
        // The host offers ed25519 first; the user only knows its ECDSA key.
        fixture.trust(fixture.ecdsaKey);
        SshTunnel tunnel({"fixture", {}, 0}, fixture.options());
        tunnel.setHostKeyPrompt([](const HostKey&, std::function<void(bool)>) { QFAIL("asked"); });
        tunnel.open();
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::Connected, patience);
        QCOMPARE(tunnel.fingerprint(), fixture.ecdsaKey.fingerprint());
        QCOMPARE(tunnel.hostKeyStatus(), HostKeyStatus::Verified);
    }

    void aSecondProbeFindsAChangedOrMissingKeyType() {
        SshFixture fixture;
        if (!fixture.start())
            QSKIP(qPrintable(fixture.skip));
        // A different ECDSA key is known: the second probe sees the host's own and refuses it.
        fixture.trust(publicKey(fixture.path("other_ecdsa"), {}));
        SshTunnel tunnel({"fixture", {}, 0}, fixture.options());
        tunnel.open();
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::HostKeyMismatch, patience);
        QCOMPARE(tunnel.fingerprint(), fixture.ecdsaKey.fingerprint());
        // Only an RSA key is known and the host offers none: the first probe's key is named.
        fixture.trust(publicKey(fixture.path("other_rsa"), {}));
        tunnel.open();
        QCOMPARE(tunnel.hostKeyStatus(), HostKeyStatus::None);
        QVERIFY(tunnel.fingerprint().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::HostKeyMismatch, patience);
        QCOMPARE(tunnel.fingerprint(), fixture.hostKey.fingerprint());
        QCOMPARE(tunnel.hostKeyStatus(), HostKeyStatus::Mismatch);
        QCOMPARE(fixture.daemonConnections.size(), 0);
    }

    void anUnreachableSecondProbeIsOfflineNotAMismatch() {
        SshFixture fixture;
        if (!fixture.start())
            QSKIP(qPrintable(fixture.skip));
        fixture.trust(fixture.ecdsaKey);
        // The host goes away between the two probes (while known_hosts is read).
        const auto keygen = fixture.path("keygen.sh");
        QVERIFY(writeFile(keygen, QStringLiteral("#!/bin/sh\nkill %1 2>/dev/null\n"
                                                 "exec ssh-keygen \"$@\"\n")
                                      .arg(fixture.sshd.processId())
                                      .toUtf8()));
        QVERIFY(
            QFile::setPermissions(keygen, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        auto options = fixture.options();
        options.keygenProgram = keygen;
        options.connectTimeoutSeconds = 2;
        SshTunnel tunnel({"fixture", {}, 0}, options);
        QList<SshTunnel::State> states;
        QObject::connect(&tunnel, &SshTunnel::stateChanged, &tunnel,
                         [&](SshTunnel::State state) { states.append(state); });
        tunnel.open();
        QTRY_VERIFY_WITH_TIMEOUT(states.contains(SshTunnel::State::Offline), patience);
        QVERIFY(!states.contains(SshTunnel::State::HostKeyMismatch));
        QVERIFY(tunnel.hostKeyStatus() != HostKeyStatus::Mismatch);
        QTRY_VERIFY_WITH_TIMEOUT(tunnel.reconnectAttempts() >= 1, patience);
        tunnel.close();
    }

    void aRevokedOrCaTrustedKeyIsDecidedByTheKnownHostsMarkers() {
        SshFixture fixture;
        if (!fixture.start())
            QSKIP(qPrintable(fixture.skip));
        // A revoked key is never offered for acceptance.
        fixture.trust(fixture.hostKey, "@revoked");
        SshTunnel tunnel({"fixture", {}, 0}, fixture.options());
        bool asked = false;
        tunnel.setHostKeyPrompt([&](const HostKey&, std::function<void(bool)>) { asked = true; });
        tunnel.open();
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::HostKeyMismatch, patience);
        QCOMPARE(tunnel.lastError(), "The host key is revoked.");
        QVERIFY(!asked);
        QVERIFY(!QFile::exists(fixture.path("ketplus/known_hosts")));
        // A CA line for the host hands the check to ssh, which needs a certificate this host
        // does not present: it fails closed rather than prompting.
        fixture.trust(publicKey(fixture.path("other"), {}), "@cert-authority");
        tunnel.open();
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::HostKeyMismatch, patience);
        QVERIFY(!asked);
        QCOMPARE(fixture.daemonConnections.size(), 0);
    }

    void aDaemonTokenOthersCanReadIsRefused() {
        SshFixture fixture;
        if (!fixture.start())
            QSKIP(qPrintable(fixture.skip));
        fixture.trust(fixture.hostKey);
        QVERIFY(QFile::setPermissions(fixture.path("local.token"),
                                      QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther));
        SshTunnel tunnel({"fixture", {}, 0}, fixture.options());
        tunnel.open();
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::DaemonTokenInsecure, patience);
        QTest::qWait(300);
        QCOMPARE(tunnel.reconnectAttempts(), 0);
        QVERIFY(tunnel.daemonCredential().isEmpty());
        QCOMPARE(fixture.daemonConnections.size(), 0);
        // No token at all: the daemon is not installed or not initialised.
        QVERIFY(QFile::remove(fixture.path("local.token")));
        tunnel.open();
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), SshTunnel::State::DaemonSocketMissing, patience);
        QCOMPARE(tunnel.lastError(), "KetPlus daemon not installed or not initialised");
        QCOMPARE(fixture.daemonConnections.size(), 0);
        tunnel.close();
    }
};

QTEST_GUILESS_MAIN(SshTunnelTest)
#include "SshTunnelTest.moc"
