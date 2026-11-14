#pragma once

#include "MachineBackend.h"
#include "remote/LinuxRelayMachines.h"

// A Relay link for Connections' tests: its backend follows connect and disconnect at once, as a
// reachable relay would, and the test sets the pending commands.
namespace ketplus::testing {

using ConnectionState = remote::MachineBackend::ConnectionState;

class FakeRelayBackend final : public remote::MachineBackend {
  public:
    explicit FakeRelayBackend(QString id, QObject* parent) : MachineBackend(parent), id_(id) {}
    QString machineId() const override { return id_; }
    Route route() const override { return Route::Relay; }
    ConnectionState connectionState() const override { return state_; }
    QString connectionReason() const override { return reason_; }
    void setState(ConnectionState state, const QString& reason = {}) {
        state_ = state;
        reason_ = reason;
        emit connectionStateChanged(state, reason);
    }

    void listRepositories(Callback) override {}
    void listWorktrees(const QString&, qint64, const QJsonObject&, Callback) override {}
    void listConversations(const QJsonObject&, Callback) override {}
    void transcriptWindow(const QString&, qint64, const QJsonObject&, Callback) override {}
    void transcriptPage(const QString&, qint64, const QJsonObject&, Callback) override {}
    void transcriptLargeEvent(const QString&, qint64, const QJsonObject&, Callback) override {}
    void createSession(const QJsonObject&, Callback) override {}
    void startTurn(const QString&, qint64, const QJsonObject&, Callback) override {}
    void steerTurn(const QString&, qint64, const QJsonObject&, Callback) override {}
    void stopTurn(const QString&, qint64, Callback) override {}
    void pendingApprovals(const QString&, qint64, const QString&, Callback) override {}
    void resolveApproval(const QString&, qint64, const QString&, const QJsonObject&,
                         Callback) override {}
    void answerQuestion(const QString&, qint64, const QString&, const QJsonObject&,
                        Callback) override {}

  private:
    QString id_;
    ConnectionState state_{ConnectionState::Offline};
    QString reason_;
};

class FakeRelayLink final : public LinuxRelayLink {
  public:
    explicit FakeRelayLink(const QString& relayId)
        : backend_(new FakeRelayBackend(relayId, this)) {}
    remote::MachineBackend* backend() override { return backend_; }
    void connectToRelay() override {
        ++connects;
        backend_->setState(ConnectionState::Connecting, QStringLiteral("Connecting to the relay."));
        if (autoConnect)
            backend_->setState(ConnectionState::Connected);
    }
    void disconnectFromRelay() override {
        ++disconnects;
        backend_->setState(ConnectionState::Offline,
                           QStringLiteral("Disconnected from the relay."));
    }
    int pendingCommands() const override { return pending; }
    void setPending(int count) {
        pending = count;
        emit pendingChanged();
    }

    FakeRelayBackend* backend_;
    bool autoConnect{true};
    int connects{0};
    int disconnects{0};
    int pending{0};
};

} // namespace ketplus::testing
