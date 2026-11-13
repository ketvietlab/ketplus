#pragma once

// Test doubles for the SSH machine backend (ADR 0119): a remote daemon on a local socket that
// answers daemon.hello with a host identity, and a link that plays SshTunnel in front of it.

#include "ssh/SshHostKeys.h"
#include "ssh/SshMachineBackend.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QTest>

#include <memory>
#include <utility>

namespace ketplus::remote::ssh::testing {

inline constexpr auto token = "remote-daemon-token";

// The remote daemon: answers daemon.hello with a host key fingerprint and records the rest.
class FakeDaemon final : public QObject {
  public:
    explicit FakeDaemon(const QString& name) {
        QLocalServer::removeServer(name);
        server.listen(name);
        connect(&server, &QLocalServer::newConnection, this, [this] {
            while (QLocalSocket* socket = server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QLocalSocket::readyRead, this,
                        [this, socket, buffer] { read(socket, *buffer); });
                connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    void read(QLocalSocket* socket, QByteArray& buffer) {
        buffer.append(socket->readAll());
        while (buffer.size() >= 4) {
            const quint32 size = (quint32(uchar(buffer[0])) << 24U) |
                                 (quint32(uchar(buffer[1])) << 16U) |
                                 (quint32(uchar(buffer[2])) << 8U) | quint32(uchar(buffer[3]));
            if (buffer.size() < 4 + qsizetype(size))
                return;
            const QJsonObject request = QJsonDocument::fromJson(buffer.mid(4, size)).object();
            buffer.remove(0, 4 + qsizetype(size));
            const QString type = request.value("message_type").toString();
            const QJsonObject payload = request.value("payload").toObject();
            if (type == "daemon.hello") {
                ++hellos;
                if (payload.value("token").toString() != token) {
                    answer(socket, request, {{"ok", false}, {"error_message", "bad token"}});
                    continue;
                }
                QJsonObject hello{{"ok", true},
                                  {"protocol_minor", 1},
                                  {"capabilities", QJsonArray::fromStringList(capabilities)},
                                  {"features", QJsonArray::fromStringList(features)}};
                if (!fingerprint.isEmpty())
                    hello.insert("host_key_fingerprint", fingerprint);
                answer(socket, request, hello);
                continue;
            }
            requests.append(type);
            payloads.append(request.value("payload").toObject());
            if (type == "workspace.list")
                answer(socket, request,
                       {{"ok", true},
                        {"workspaces",
                         QJsonArray{QJsonObject{{"path", "/home/dev/app"}, {"available", true}}}}});
            else
                answer(socket, request, {{"ok", true}});
        }
    }

    static void answer(QLocalSocket* socket, const QJsonObject& request,
                       const QJsonObject& payload) {
        const QByteArray body =
            QJsonDocument(QJsonObject{{"correlation_id", request.value("message_id")},
                                      {"message_type", request.value("message_type")},
                                      {"payload", payload}})
                .toJson(QJsonDocument::Compact);
        QByteArray frame(4, Qt::Uninitialized);
        const auto size = quint32(body.size());
        frame[0] = char((size >> 24U) & 0xffU);
        frame[1] = char((size >> 16U) & 0xffU);
        frame[2] = char((size >> 8U) & 0xffU);
        frame[3] = char(size & 0xffU);
        socket->write(frame + body);
        socket->flush();
    }

    QLocalServer server;
    QString fingerprint{"daemon-key-A"};
    QStringList features{"daemon.host_identity/1"};
    QStringList capabilities;
    QStringList requests;
    QList<QJsonObject> payloads;
    int hellos{0};
};

// Plays SshTunnel: open() connects to the fake daemon and reports Connected; tests can also set
// any state directly.
class FakeLink final : public SshLink {
  public:
    explicit FakeLink(QString name) : name_(std::move(name)) {}

    void open() override {
        ++opens;
        if (!autoConnect)
            return;
        setTunnelState(SshTunnel::State::Connecting);
        auto* socket = new QLocalSocket(this);
        connect(socket, &QLocalSocket::connected, this, [this, socket] {
            socket_ = socket;
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
                if (socket_ == socket)
                    setTunnelState(SshTunnel::State::Offline);
            });
            setTunnelState(SshTunnel::State::Connected);
        });
        socket->connectToServer(name_);
    }
    void close() override {
        if (socket_)
            socket_->abort();
        socket_.clear();
        setTunnelState(SshTunnel::State::Idle);
    }
    [[nodiscard]] SshTunnel::State state() const override { return state_; }
    [[nodiscard]] QString fingerprint() const override { return hostKey; }
    [[nodiscard]] bool fingerprintVerified() const override {
        return state_ != SshTunnel::State::HostKeyMismatch;
    }
    [[nodiscard]] QLocalSocket* connection() const override {
        return state_ == SshTunnel::State::Connected ? socket_.data() : nullptr;
    }
    [[nodiscard]] QByteArray daemonCredential() const override { return token; }
    [[nodiscard]] QString knownHostsFile() const override { return knownHosts; }
    void setHostKeyPrompt(SshTunnel::HostKeyPrompt next) override { prompt = std::move(next); }

    // What SshTunnel does with an unknown key: ask, and record it in its file when accepted.
    void offerKey(const HostKey& key) {
        QVERIFY(prompt);
        prompt(key, [this, key](const bool accepted) {
            if (accepted)
                QVERIFY(recordHostKey(knownHosts, key));
        });
    }

    void setTunnelState(SshTunnel::State state) {
        if (state != SshTunnel::State::Connected && socket_) {
            QPointer<QLocalSocket> socket = std::exchange(socket_, {});
            socket->abort();
        }
        state_ = state;
        emit stateChanged(state);
    }
    void dropConnection() {
        if (socket_)
            socket_->abort();
    }

    bool autoConnect{true};
    // The SSH host key the link reports as verified.
    QString hostKey{QStringLiteral("SHA256:sshkey")};
    int opens{0};
    QString knownHosts;
    SshTunnel::HostKeyPrompt prompt;

  private:
    QString name_;
    SshTunnel::State state_{SshTunnel::State::Idle};
    QPointer<QLocalSocket> socket_;
};

} // namespace ketplus::remote::ssh::testing
