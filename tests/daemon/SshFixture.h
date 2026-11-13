#pragma once

#include "ssh/SshHostKeys.h"
#include "ssh/SshTunnel.h"

#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

// A loopback sshd on 127.0.0.1 with throwaway host and user keys, and the ssh client config that
// reaches it. Without `listening`, no fake daemon binds daemon.sock and a real one can. No real
// host, account, agent or ~/.ssh file is used. Shared by the SSH tunnel and backend tests.
namespace ketplus::remote::ssh::testing {

inline QByteArray
runTool(const QString& program, const QStringList& arguments,
        const QProcessEnvironment& environment = QProcessEnvironment::systemEnvironment()) {
    QProcess process;
    process.setProcessEnvironment(environment);
    process.start(program, arguments);
    if (!process.waitForFinished(20000) || process.exitCode() != 0)
        return {};
    return process.readAllStandardOutput();
}
inline bool keygen(const QString& path, const QString& type = "ed25519") {
    QFile::remove(path);
    QFile::remove(path + ".pub");
    QProcess process;
    process.start("ssh-keygen", {"-q", "-t", type, "-N", "", "-C", "fixture", "-f", path});
    return process.waitForFinished(20000) && process.exitCode() == 0;
}
inline HostKey publicKey(const QString& path, const QString& name) {
    QFile file(path + ".pub");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const auto fields = QString::fromUtf8(file.readAll()).split(' ', Qt::SkipEmptyParts);
    return fields.size() >= 2 ? HostKey{name, fields.at(0), fields.at(1)} : HostKey{};
}
inline bool writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(contents) == contents.size();
}

// Every message logged while a test runs.
inline QStringList& logged() {
    static QStringList messages;
    return messages;
}
inline void capture(QtMsgType, const QMessageLogContext&, const QString& message) {
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
    QString extraServerEnvironment;

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
                                 "SetEnv KETPLUS_DAEMON_SOCKET=%6 KETPLUS_DAEMON_TOKEN_FILE=%7%9\n"
                                 "LogLevel ERROR\nBanner %8\n")
                      .arg(port)
                      .arg(path("host"), path("sshd.pid"), path("authorized_keys"), user,
                           path("daemon.sock"), path("local.token"), path("banner"))
                      .arg(extraServerEnvironment)
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

inline constexpr int patience = 30000;
} // namespace ketplus::remote::ssh::testing
