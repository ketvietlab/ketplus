#include "DaemonMachineBackend.h"

#include "DaemonClient.h"

#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QPointer>

#include <utility>

namespace ketplus::remote {

namespace {

QString notRunning() { return QStringLiteral("The ketPlus daemon is not running."); }

} // namespace

DaemonMachineBackend::DaemonMachineBackend(DaemonClient* client, QObject* parent)
    : DaemonMachineBackend(client, QStringLiteral("local"), Route::Local, parent) {}

DaemonMachineBackend::DaemonMachineBackend(DaemonClient* client, QString machineId,
                                           const Route route, QObject* parent)
    : DaemonMachineBackend(
          std::move(machineId), route,
          [client = QPointer<DaemonClient>(client)](const QString& type, const QJsonObject& payload,
                                                    const QString& sessionId,
                                                    const qint64 generation, Callback callback) {
              if (!client) {
                  callback({}, notRunning());
                  return;
              }
              client->call(type, payload, sessionId, generation, std::move(callback));
          },
          parent) {
    if (!client) {
        setTransportUnavailable(notRunning());
        return;
    }
    connect(client, &DaemonClient::readyChanged, this,
            [this](const bool ready) { setTransportReady(ready); });
    // Sent after readyChanged(false) when a Connected session drops; DaemonClient reconnects.
    connect(client, &DaemonClient::disconnected, this,
            [this](const QString& reason) { setTransportReady(false, reason); });
    connect(client, &DaemonClient::eventReceived, this, &MachineBackend::eventReceived);
    connect(client, &QObject::destroyed, this, [this] { setTransportUnavailable(notRunning()); });
    if (client->isReady())
        setTransportReady(true);
    // Only This Mac launches its own daemon; an SSH route reports "not installed" itself.
    else if (route_ == Route::Local && !client->isAvailable())
        setTransportUnavailable(QStringLiteral("The ketPlus daemon is not installed."));
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
    request(QStringLiteral("workspace.list"), {}, {}, -1,
            [callback = std::move(callback)](const QJsonObject& result, const QString& error) {
                if (!error.isEmpty()) {
                    callback({}, error);
                    return;
                }
                callback(groupWorkspacesByRepository(result), {});
            });
}

QJsonObject DaemonMachineBackend::groupWorkspacesByRepository(const QJsonObject& workspaceList) {
    // ADR 0119 "Repository grain": workspace.list names registered checkouts by path, linked
    // worktrees included. The neutral shape is one row per repository with its checkouts under
    // `worktrees`, as the Relay groups by repository id. A daemon that does not name a
    // checkout's repository (`repository_path`) makes that checkout its own repository.
    QJsonArray repositories;
    QHash<QString, qsizetype> indexByRepository;
    for (const auto& value : workspaceList.value(QStringLiteral("workspaces")).toArray()) {
        const auto workspace = value.toObject();
        const QString path = workspace.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            continue;
        QString repositoryPath = workspace.value(QStringLiteral("repository_path")).toString();
        if (repositoryPath.isEmpty())
            repositoryPath = path;
        const bool available = workspace.value(QStringLiteral("available")).toBool();
        const QJsonObject worktree{{QStringLiteral("id"), path},
                                   {QStringLiteral("label"), QFileInfo(path).fileName()},
                                   {QStringLiteral("path"), path},
                                   {QStringLiteral("available"), available}};
        if (!indexByRepository.contains(repositoryPath)) {
            indexByRepository.insert(repositoryPath, repositories.size());
            repositories.append(
                QJsonObject{{QStringLiteral("id"), repositoryPath},
                            {QStringLiteral("label"), QFileInfo(repositoryPath).fileName()},
                            {QStringLiteral("path"), repositoryPath},
                            {QStringLiteral("available"), false},
                            {QStringLiteral("worktrees"), QJsonArray{}}});
        }
        const qsizetype index = indexByRepository.value(repositoryPath);
        auto repository = repositories.at(index).toObject();
        auto worktrees = repository.value(QStringLiteral("worktrees")).toArray();
        worktrees.append(worktree);
        repository.insert(QStringLiteral("worktrees"), worktrees);
        // A repository is available while any of its checkouts is.
        if (available)
            repository.insert(QStringLiteral("available"), true);
        repositories.replace(index, repository);
    }
    return {{QStringLiteral("repositories"), repositories}};
}

void DaemonMachineBackend::listWorktrees(const QString& sessionId, const qint64 generation,
                                         const QJsonObject& request, Callback callback) {
    this->request(QStringLiteral("session.worktrees"), request, sessionId, generation,
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

void DaemonMachineBackend::transcriptLargeEvent(const QString& sessionId, const qint64 generation,
                                                const QJsonObject& request, Callback callback) {
    this->request(QStringLiteral("session.transcript_large_event"), request, sessionId, generation,
                  std::move(callback));
}

void DaemonMachineBackend::createSession(const QJsonObject& request, Callback callback) {
    // Locally a worktree id is its path, and the first turn creates the conversation.
    QJsonObject payload{
        {QStringLiteral("prompt"), request.value(QStringLiteral("prompt"))},
        {QStringLiteral("worktree_path"), request.value(QStringLiteral("worktree_id"))}};
    if (request.contains(QStringLiteral("options")))
        payload.insert(QStringLiteral("options"), request.value(QStringLiteral("options")));
    this->request(QStringLiteral("session.start_turn"), payload, {}, 0, std::move(callback));
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
