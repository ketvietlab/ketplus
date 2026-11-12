#include "DaemonMachineBackend.h"

#include "DaemonClient.h"

#include <QPointer>

#include <utility>

namespace ketplus::remote {

DaemonMachineBackend::DaemonMachineBackend(DaemonClient* client, QObject* parent)
    : DaemonMachineBackend(
          QStringLiteral("local"), Route::Local,
          [client = QPointer<DaemonClient>(client)](const QString& type, const QJsonObject& payload,
                                                    const QString& sessionId,
                                                    const qint64 generation, Callback callback) {
              if (!client) {
                  callback({}, QStringLiteral("KetPlus Daemon is not running."));
                  return;
              }
              client->call(type, payload, sessionId, generation, std::move(callback));
          },
          parent) {
    if (!client) {
        setTransportUnavailable(QStringLiteral("KetPlus Daemon is not running."));
        return;
    }
    connect(client, &DaemonClient::readyChanged, this,
            [this](const bool ready) { setTransportReady(ready); });
    // Sent after readyChanged(false) when a Connected session drops; DaemonClient reconnects.
    connect(client, &DaemonClient::disconnected, this,
            [this](const QString& reason) { setTransportReady(false, reason); });
    connect(client, &DaemonClient::eventReceived, this, &MachineBackend::eventReceived);
    connect(client, &QObject::destroyed, this,
            [this] { setTransportUnavailable(QStringLiteral("KetPlus Daemon is not running.")); });
    if (client->isReady())
        setTransportReady(true);
    else if (!client->isAvailable())
        setTransportUnavailable(QStringLiteral("KetPlus Daemon is not installed."));
}

DaemonMachineBackend::DaemonMachineBackend(QString machineId, const Route route, DaemonCall call,
                                           QObject* parent)
    : MachineBackend(parent), machineId_(std::move(machineId)), route_(route),
      call_(std::move(call)) {}

void DaemonMachineBackend::setTransportReady(const bool ready, const QString& reason) {
    if (ready) {
        everConnected_ = true;
        setState(ConnectionState::Connected, {});
        return;
    }
    if (everConnected_) {
        setState(ConnectionState::Reconnecting,
                 reason.isEmpty() && state_ == ConnectionState::Reconnecting ? reason_ : reason);
        return;
    }
    setState(ConnectionState::Connecting, reason);
}

void DaemonMachineBackend::setTransportUnavailable(const QString& reason) {
    setState(ConnectionState::Offline, reason);
}

void DaemonMachineBackend::setState(const ConnectionState state, const QString& reason) {
    if (state_ == state && reason_ == reason)
        return;
    state_ = state;
    reason_ = reason;
    emit connectionStateChanged(state_, reason_);
}

void DaemonMachineBackend::request(const QString& type, const QJsonObject& payload,
                                   const QString& sessionId, const qint64 generation,
                                   Callback callback) {
    call_(type, payload, sessionId, generation, std::move(callback));
}

void DaemonMachineBackend::listRepositories(Callback callback) {
    request(QStringLiteral("workspace.list"), {}, {}, -1, std::move(callback));
}

void DaemonMachineBackend::listWorktrees(const QString& sessionId, const qint64 generation,
                                         const int limit, Callback callback) {
    QJsonObject payload;
    if (limit > 0)
        payload.insert(QStringLiteral("limit"), limit);
    request(QStringLiteral("session.worktrees"), payload, sessionId, generation,
            std::move(callback));
}

void DaemonMachineBackend::listConversations(const QJsonObject& options, Callback callback) {
    request(QStringLiteral("session.list"), options, {}, 0, std::move(callback));
}

void DaemonMachineBackend::transcriptWindow(const QString& sessionId, const qint64 generation,
                                            const QJsonObject& request, Callback callback) {
    this->request(QStringLiteral("session.transcript_window"), request, sessionId, generation,
                  std::move(callback));
}

void DaemonMachineBackend::transcriptPage(const QString& sessionId, const qint64 generation,
                                          const QJsonObject& request, Callback callback) {
    this->request(QStringLiteral("session.transcript_page"), request, sessionId, generation,
                  std::move(callback));
}

void DaemonMachineBackend::startTurn(const QString& sessionId, const qint64 generation,
                                     const QJsonObject& request, Callback callback) {
    this->request(QStringLiteral("session.start_turn"), request, sessionId, generation,
                  std::move(callback));
}

void DaemonMachineBackend::steerTurn(const QString& sessionId, const qint64 generation,
                                     const QJsonObject& request, Callback callback) {
    this->request(QStringLiteral("session.steer"), request, sessionId, generation,
                  std::move(callback));
}

void DaemonMachineBackend::stopTurn(const QString& sessionId, const qint64 generation,
                                    Callback callback) {
    request(QStringLiteral("session.interrupt"), {}, sessionId, generation, std::move(callback));
}

void DaemonMachineBackend::pendingApprovals(const QString& sessionId, const qint64 generation,
                                            const QString& afterId, Callback callback) {
    QJsonObject payload;
    if (!afterId.isEmpty())
        payload.insert(QStringLiteral("after_id"), afterId);
    request(QStringLiteral("session.pending_approvals"), payload, sessionId, generation,
            std::move(callback));
}

void DaemonMachineBackend::resolveApproval(const QString& sessionId, const qint64 generation,
                                           const QString& approvalId, const QJsonObject& resolution,
                                           Callback callback) {
    request(
        QStringLiteral("approval.resolve"),
        {{QStringLiteral("approval_id"), approvalId}, {QStringLiteral("resolution"), resolution}},
        sessionId, generation, std::move(callback));
}

void DaemonMachineBackend::answerQuestion(const QString& sessionId, const qint64 generation,
                                          const QString& questionId, const QJsonObject& resolution,
                                          Callback callback) {
    // The daemon keys questions by the same approval id as approvals (CodexAppServer.cpp).
    request(
        QStringLiteral("question.answer"),
        {{QStringLiteral("approval_id"), questionId}, {QStringLiteral("resolution"), resolution}},
        sessionId, generation, std::move(callback));
}

} // namespace ketplus::remote
