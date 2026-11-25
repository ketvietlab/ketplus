#include <QtGlobal>
// POSIX-only; Windows builds NativeJobUnsupported.cpp instead.
#if !defined(Q_OS_WIN)
#include "runtime/executor/LinuxJobContainment.h"
#include "runtime/executor/NativeJobClient.h"
#include "runtime/executor/NativeJobControlAddress.h"
#include "runtime/executor/NativeJobLease.h"
#include "runtime/executor/NativeProcessIdentity.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <unistd.h>

namespace ketplus::daemon::executor {
NativeJobClient::NativeJobClient(QString executable, QString directory, QString taskId,
                                 QString lease, QObject* parent)
    : QObject(parent), executable_(std::move(executable)), directory_(std::move(directory)),
      taskId_(std::move(taskId)), lease_(std::move(lease)) {
    socket_.setReadBufferSize(2 * 1024 * 1024 + 1);
    connect(&socket_, &QLocalSocket::connected, this, [this] {
        input_.clear();
        authenticated_ = false;
        send({{"op", "auth"}, {"lease_id", lease_}});
    });
    connect(&socket_, &QLocalSocket::readyRead, this, [this] { receive(); });
    connect(&socket_, &QLocalSocket::disconnected, this, [this] { channelClosed(); });
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
}

NativeJobClient::~NativeJobClient() {
    // No process ownership in this QObject: the independent supervisor's lease
    // reclaims work if its daemon dies or abandons the connection.
    socket_.abort();
}

bool NativeJobClient::validateDirectory(QString* error) const {
    const QFileInfo directory(directory_);
    if (taskId_.isEmpty() || lease_.isEmpty() || !directory.isDir() || directory.isSymLink() ||
        directory.canonicalFilePath() != directory_ || directory.ownerId() != geteuid() ||
        (directory.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                                    QFile::ReadOther | QFile::WriteOther | QFile::ExeOther))) {
        if (error)
            *error = "The job directory must be canonical and accessible only to its owner.";
        return false;
    }
    return true;
}

QJsonObject NativeJobClient::receipt(QString* error) const {
    if (!validateDirectory(error))
        return {};
    QFile file(directory_ + "/executor.json");
    if (file.size() > 2 * 1024 * 1024 || !file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = "The executor receipt is unavailable or too large.";
        return {};
    }
    const auto record = QJsonDocument::fromJson(file.readAll()).object();
    if (record.value("schema_version").toInt() != 1 || record.value("task_id") != taskId_ ||
        record.value("executor_lease_identity") != lease_) {
        if (error)
            *error = "The executor receipt does not match its durable job identity.";
        return {};
    }
    return record;
}

bool NativeJobClient::start(const QJsonObject& execution, qint64 remainingTimeoutMs,
                            CommitHandshake commitHandshake, Event event, QString* error) {
    if (timer_.isActive() || remainingTimeoutMs <= 0 || !validateDirectory(error))
        return false;
#if !defined(Q_OS_MACOS) && !defined(Q_OS_LINUX)
    if (error)
        *error = "Native best-effort execution is supported only on macOS.";
    return false;
#endif
    if (QFileInfo::exists(directory_ + "/executor.json") ||
        QFileInfo::exists(controlAddress(directory_, lease_))) {
        if (error)
            *error = "An existing executor must be reconciled, never launched again.";
        return false;
    }
    execution_ = execution;
#ifdef Q_OS_LINUX
    const auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto& key : environment.keys())
        inheritedEnvironment_.insert(key, environment.value(key));
    if (QJsonDocument(inheritedEnvironment_).toJson(QJsonDocument::Compact).size() +
            QJsonDocument(execution).toJson(QJsonDocument::Compact).size() >
        192 * 1024) {
        inheritedEnvironment_ = {};
        execution_ = {};
        if (error)
            *error = "The inherited process environment exceeds the private launch limit.";
        return false;
    }
#endif
    remainingTimeout_ = remainingTimeoutMs;
    commitHandshake_ = std::move(commitHandshake);
    event_ = std::move(event);
#ifdef Q_OS_LINUX
    // An ambiguous manager response must still be reconciled by the durable
    // unit/lease identity. It is not evidence that the helper never launched.
    managerPending_ = true;
    linuxStartSupervisorAsync(executable_, directory_, taskId_, lease_, this, [this] {
        managerPending_ = false;
        tick();
    });
#else
    QProcess supervisor;
    supervisor.setProgram(executable_);
    supervisor.setArguments({directory_, taskId_, lease_});
    supervisor.setStandardInputFile(QProcess::nullDevice());
    supervisor.setStandardOutputFile(QProcess::nullDevice());
    supervisor.setStandardErrorFile(QProcess::nullDevice());
    if (!supervisor.startDetached()) {
        if (error)
            *error = supervisor.errorString();
        return false;
    }
#endif
    launchedSupervisor_ = true;
    clock_.start();
    timer_.start();
    tick();
    return true;
}

bool NativeJobClient::reconnect(Event event, QString* error) {
    if (timer_.isActive() || !validateDirectory(error))
        return false;
    event_ = std::move(event);
    lastReceipt_ = receipt();
    clock_.start();
    timer_.start();
    tick();
    return true;
}

void NativeJobClient::connectSocket() {
    if (socket_.state() != QLocalSocket::UnconnectedState ||
        clock_.elapsed() - lastConnectionAttempt_ < 500)
        return;
    lastConnectionAttempt_ = clock_.elapsed();
    const auto record = receipt();
#ifdef Q_OS_LINUX
    if (managerPending_)
        return;
    managerPending_ = true;
    linuxUnitPropertiesAsync(
        linuxJobUnit(taskId_, lease_), this, [this, record](const QJsonObject& service) {
            managerPending_ = false;
            if (acknowledged_ || socket_.state() != QLocalSocket::UnconnectedState ||
                !linuxReceiptTrusted(taskId_, lease_, record, service))
                return;
            trustedLinuxReceipt_ = record;
            connectVerified(record);
        });
#else
    connectVerified(record);
#endif
}

void NativeJobClient::connectVerified(const QJsonObject& record) {
    const auto self = record.value("supervisor_identity").toObject();
    const auto identity = inspect(self.value("pid").toInteger());
    if (!identity || identity->birth != self.value("birth").toString() || identity->zombie)
        return;
    lastReceipt_ = record;
    socket_.connectToServer(controlAddress(directory_, lease_));
}

// EOF/HUP on the control channel is noticed at once. If the supervisor process is
// gone, recovery starts now rather than after the lease-length silence window; a live but
// silent peer still falls back to that window (ping/lease).
void NativeJobClient::channelClosed() {
    authenticated_ = false;
    if (acknowledged_ || !timer_.isActive())
        return;
    const auto record = receipt();
    const auto self = record.value("supervisor_identity").toObject();
    const auto identity = inspect(self.value("pid").toInteger());
    const bool alive = identity && identity->birth == self.value("birth").toString() &&
                       !identity->zombie;
    if (!alive && !self.isEmpty() && clock_.elapsed() - lastMessage_ < NativeJobLeaseMs + 500)
        lastMessage_ = clock_.elapsed() - NativeJobLeaseMs - 500;
    QTimer::singleShot(0, this, [this] { tick(); });
}

void NativeJobClient::send(const QJsonObject& message) {
    if (socket_.state() == QLocalSocket::ConnectedState)
        socket_.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void NativeJobClient::receive() {
    input_.append(socket_.readAll());
    if (input_.size() > 2 * 1024 * 1024) {
        socket_.abort();
        return;
    }
    for (auto end = input_.indexOf('\n'); end >= 0; end = input_.indexOf('\n')) {
        const auto value = QJsonDocument::fromJson(input_.left(end)).object();
        input_.remove(0, end + 1);
        message(value);
    }
}

void NativeJobClient::message(const QJsonObject& value) {
    const auto type = value.value("event").toString();
    if (type == "pong") {
        lastMessage_ = clock_.elapsed();
        return;
    }
    if (type == "error" && authenticated_) {
        stop("interrupted");
        return;
    }
    if (value.value("task_id") != taskId_ || value.value("executor_lease_identity") != lease_)
        return;
#ifdef Q_OS_LINUX
    const auto certified = trustedLinuxReceipt_.value("supervisor_identity").toObject();
    const auto claimed = value.value("supervisor_identity").toObject();
    if (type == "ready" &&
        (trustedLinuxReceipt_.isEmpty() || certified.value("pid") != claimed.value("pid") ||
         certified.value("birth") != claimed.value("birth") ||
         trustedLinuxReceipt_.value("linux_boundary") != value.value("linux_boundary") ||
         !linuxBoundaryMatches(value.value("linux_boundary").toObject()))) {
        socket_.abort();
        return;
    }
    if (type == "ready")
        recoveryIdentity_.insert("linux_boundary", value.value("linux_boundary"));
    const auto boundary = recoveryIdentity_.value("linux_boundary").toObject();
    if (type == "done" &&
        (!linuxBoundaryEmpty(boundary) || value.value("cleanup_status") != "verified")) {
        socket_.abort();
        return;
    }
#endif
    lastMessage_ = clock_.elapsed();
    lastReceipt_ = value;
    if (type == "ready") {
        authenticated_ = true;
        if (!value.value("state").toString().isEmpty()) {
            auto terminal = value;
            terminal.insert("event", "done");
            message(terminal);
            return;
        }
        if (!stopReason_.isEmpty()) {
            send({{"op", "stop"}, {"reason", stopReason_}});
        } else if (launchedSupervisor_ && !sentStart_ &&
                   !value.value("launch_attempted").toBool()) {
            // Durable host handshake must commit before the supervisor receives any command.
            if (!commitHandshake_ || !commitHandshake_(value)) {
                stop("interrupted");
                return;
            }
            sentStart_ = true;
            const auto remaining = remainingTimeout_ - clock_.elapsed();
            if (remaining <= 0) {
                stop("timed_out");
                return;
            }
            send({{"op", "start"},
                  {"execution", execution_},
                  {"base_environment", inheritedEnvironment_},
                  {"remaining_timeout_ms", remaining}});
            execution_ = {}; // Secrets supplied for this launch are not retained or journaled.
            inheritedEnvironment_ = {};
        } else if (!value.value("launch_attempted").toBool()) {
            stop("interrupted"); // A recovered pre-launch intent is never replayed.
        }
    }
    if (type == "done")
        finished_ = true;
    if (event_)
        event_(value);
}

void NativeJobClient::stop(const QString& reason) {
    if (stopReason_.isEmpty())
        stopReason_ = reason;
    if (authenticated_ && (!finished_ || lastReceipt_.value("state") == "needs_attention"))
        send({{"op", "stop"}, {"reason", stopReason_}});
}

void NativeJobClient::acknowledge() {
    if (!finished_)
        return;
    acknowledged_ = true;
    send({{"op", "ack"}});
    socket_.flush();
    timer_.stop();
}

void NativeJobClient::tick() {
    if (acknowledged_)
        return;
    connectSocket();
    if (authenticated_)
        send({{"op", "ping"}});
    if (clock_.elapsed() - lastMessage_ >= 1500 && socket_.state() == QLocalSocket::ConnectedState)
        socket_.abort();
    // A live supervisor gets its full lease/grace window to finish cleanup and
    // write a result. Recovery never calls start() to reconstruct a missing result.
    if (clock_.elapsed() - lastMessage_ >= NativeJobLeaseMs + 500 &&
        clock_.elapsed() - lastRecoveryAttempt_ >= 1000) {
        lastRecoveryAttempt_ = clock_.elapsed();
        recoverLostSupervisor();
    }
}
} // namespace ketplus::daemon::executor
#endif // !Q_OS_WIN
