#include "ssh/SshProcess.h"
#include "ssh/SshTunnel.h"
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

// Daemon steps of SshTunnel: locating the socket and token, then the `ssh -W` bridge to a
// private local socket (docs/ssh-transport.md, steps 4 and 5).
namespace ketplus::remote::ssh {

void SshTunnel::locate() {
    run(options_.sshProgram,
        verifiedOptions() +
            QStringList{target_.alias, "exec /bin/sh -c '" + remoteSocketProbe() + "'"},
        [this](bool finished, int code, const QByteArray& out, const QByteArray& err) {
            DaemonProbe probe;
            if (!finished || code != 0 || !parseDaemonProbe(out, &probe)) {
                fail(classify(err), "The host could not be reached.");
                return;
            }
            remoteSocket_ = probe.socketPath;
            if (!probe.present) {
                fail(State::DaemonSocketMissing, "The KetPlus daemon is not running on the host.");
                return;
            }
            switch (probe.token) {
            case DaemonProbe::Token::Missing:
                fail(State::DaemonSocketMissing, "KetPlus daemon not installed or not initialised");
                return;
            case DaemonProbe::Token::Permissive:
                fail(State::DaemonTokenInsecure,
                     "The daemon token on the host is readable by other users.");
                return;
            case DaemonProbe::Token::Present:
                break;
            }
            // ssh accepted the host (and its certificate, on the CA path) for this call.
            hostKeyStatus_ = HostKeyStatus::Verified;
            credential_ = std::move(probe.credential);
            forward();
        });
}

void SshTunnel::forward() {
    // `ssh -W` carries the daemon socket on the forwarder's stdio, so no forwarding is set up
    // (ClearAllForwardings stays on) and ssh binds nothing. A private local server bridges that
    // stdio to the QLocalSocket the caller reads.
    const auto local = work_->filePath("daemon.sock");
    QLocalServer::removeServer(local);
    auto* server = new QLocalServer(this);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    bridge_ = server;
    if (!server->listen(local)) {
        fail(State::Offline, "Unable to open a private local socket.");
        return;
    }
    auto* process = new QProcess(this);
    forwarder_ = process;
    const auto generation = generation_;
    connect(process, &QProcess::finished, this, [this, process, generation] {
        process->deleteLater();
        if (generation != generation_)
            return;
        const auto state = classify(process->readAllStandardError());
        fail(state, state == State::Offline ? "The SSH connection closed."
                                            : "The host refused the SSH connection.");
    });
    connect(process, &QProcess::errorOccurred, this, [this, generation](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && generation == generation_)
            fail(State::Offline, "Unable to start ssh.");
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, generation] {
        // Held in the process until the bridge has its peer.
        if (generation != generation_ || !peer_)
            return;
        peer_->write(process->readAllStandardOutput());
        carried_ = true;
        settle(generation);
    });
    connect(server, &QLocalServer::newConnection, this, [this, server, process, generation] {
        auto* peer = server->nextPendingConnection();
        if (!peer)
            return;
        if (generation != generation_ || peer_) {
            peer->abort();
            peer->deleteLater();
            return;
        }
        // One connection only; nobody else can reach the bridge.
        server->close();
        peer_ = peer;
        connect(peer, &QLocalSocket::readyRead, this, [this, peer, process, generation] {
            if (generation == generation_ && forwarder_ == process)
                process->write(peer->readAll());
        });
        if (process->bytesAvailable() > 0) {
            peer->write(process->readAllStandardOutput());
            carried_ = true;
        }
    });
    process->setProcessEnvironment(sshEnvironment());
    process->start(options_.sshProgram,
                   verifiedOptions(true) + QStringList{"-W", remoteSocket_, target_.alias});

    auto* socket = new QLocalSocket(this);
    connect(socket, &QLocalSocket::connected, this, [this, socket, generation] {
        if (generation != generation_) {
            socket->abort();
            socket->deleteLater();
            return;
        }
        socket_ = socket;
        connect(socket, &QLocalSocket::disconnected, this, [this, socket, generation] {
            if (generation == generation_ && socket_ == socket)
                lost("The daemon connection closed.");
        });
        if (carried_)
            settle(generation);
        else
            QTimer::singleShot(options_.settleMilliseconds, this,
                               [this, generation] { settle(generation); });
    });
    connect(socket, &QLocalSocket::errorOccurred, this, [this, socket, generation] {
        // After Connected, a closed connection is reported by disconnected().
        if (socket_ == socket)
            return;
        socket->deleteLater();
        if (generation == generation_)
            fail(State::Offline, "Unable to open a private local socket.");
    });
    socket->connectToServer(local);
}

void SshTunnel::settle(quint64 generation) {
    if (generation != generation_ || state_ != State::Connecting || !socket_ || !forwarder_ ||
        forwarder_->state() != QProcess::Running)
        return;
    // Only a bridge that held up resets the backoff.
    reconnects_ = 0;
    lastError_.clear();
    setState(State::Connected);
}

} // namespace ketplus::remote::ssh
