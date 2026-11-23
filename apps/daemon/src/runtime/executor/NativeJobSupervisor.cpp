#include "runtime/executor/NativeJobSupervisor.h"
#include "runtime/executor/LinuxJobContainment.h"
#include "runtime/executor/NativeJobControlAddress.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSqlError>
#include <QUuid>
#include <cerrno>
#include <csignal>
#include <unistd.h>

namespace ketplus::daemon::executor {
namespace {
volatile sig_atomic_t stopRequested = 0;
void stopSignal(int) { stopRequested = 1; }
} // namespace

bool supervisorStopRequested() { return stopRequested != 0; }

NativeJobSupervisor::NativeJobSupervisor(QString directory, QString taskId, QString lease,
                                         QObject* parent)
    : QObject(parent), directory_(std::move(directory)), taskId_(std::move(taskId)),
      lease_(std::move(lease)), connectionName_(QUuid::createUuid().toString()) {}

NativeJobSupervisor::~NativeJobSupervisor() {
    closeLaunchGate();
    output_.reset();
    outputDatabase_.close();
    outputDatabase_ = {};
    QSqlDatabase::removeDatabase(connectionName_);
}

bool NativeJobSupervisor::initialize(QString* error) {
#if !defined(Q_OS_MACOS) && !defined(Q_OS_LINUX)
    *error = "Native best-effort process execution is supported only on macOS.";
    return false;
#endif
    const QFileInfo directory(directory_);
    const auto self = inspect(QCoreApplication::applicationPid());
    if (!self || !directory.isDir() || directory.isSymLink() ||
        directory.canonicalFilePath() != directory_ || directory.ownerId() != geteuid() ||
        (directory.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                                    QFile::ReadOther | QFile::WriteOther | QFile::ExeOther))) {
        *error = "The executor needs a canonical, owner-only job directory and birth identity.";
        return false;
    }
    metadata_ = {{"task_id", taskId_},
                 {"schema_version", 1},
                 {"executor_lease_identity", lease_},
                 {"supervisor_identity", self->json()},
                 {"containment_mode", "native_best_effort"},
                 {"cleanup_status", "not_required"},
                 {"launch_attempted", false},
                 {"root_exited", false},
                 {"output_size", 0},
                 {"output_truncated", false},
                 {"executor_stopped", true}};
#ifdef Q_OS_LINUX
    metadata_.insert("containment_mode", "cgroup_v2");
    metadata_.insert("boot_id", linuxBootId());
    // The deterministic service name is committed by the daemon before launch.
    metadata_.insert("systemd_unit", linuxJobUnit(taskId_, lease_));
#endif
    // The receipt is this supervisor's first durable act. The daemon reads a missing receipt
    // as a supervisor that never started (NativeJobRecovery.cpp), so only the directory and
    // birth-identity checks above run before it, and they launch nothing.
    if (!persist())
        return false;
    // A failure before the control socket listens launched nothing: record it as a failed
    // handshake so the daemon ends the job and releases its slot instead of waiting for a user.
    const auto failBeforeListening = [&] {
#ifndef Q_OS_LINUX
        metadata_.insert("error", "handshake_failed");
        metadata_.insert("state", "failed");
        persist();
#endif
        return false;
    };
#ifdef Q_OS_LINUX
    const auto boundary = linuxCreateBoundary(taskId_, lease_, error);
    if (boundary.isEmpty())
        return false;
    metadata_.insert("linux_boundary", boundary);
#endif
    outputDatabase_ = QSqlDatabase::addDatabase("QSQLITE", connectionName_);
    outputDatabase_.setDatabaseName(directory_ + "/output.sqlite");
    outputDatabase_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=50");
    if (!outputDatabase_.open()) {
        *error = outputDatabase_.lastError().text();
        return failBeforeListening();
    }
    output_ = std::make_unique<BackgroundTaskOutput>(outputDatabase_);
    if (!output_->initialize(error))
        return failBeforeListening();
    if (!persist())
        return false;
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    if (!ensureControlDirectory(error) || !server_.listen(controlAddress(directory_, lease_))) {
        if (error->isEmpty())
            *error = server_.errorString();
        metadata_.insert("error", "handshake_failed");
        metadata_.insert("state", "failed");
        persist();
        return false;
    }
    connect(&server_, &QLocalServer::newConnection, this, [this] { acceptConnection(); });
    process_.setProcessChannelMode(QProcess::MergedChannels);
    process_.setStandardInputFile(QProcess::nullDevice());
    connect(&process_, &QProcess::started, this, [this] {
        // The child was birth-identified and journaled while blocked before exec.
        // A short command may already have exited when this Qt signal arrives.
        if (!root_)
            metadata_.insert("error", "handshake_failed");
        metadata_.insert("executor_stopped", false);
        metadata_.insert("root_started", true);
        if (!persist())
            requestStop("interrupted");
        send("running", metadata_);
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { drainOutput(); });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        drainOutput();
        rootExited_ = true;
        exitCode_ = status == QProcess::NormalExit ? code : -1;
        metadata_.insert("exit_code", exitCode_);
        metadata_.insert("root_exited", true);
        persist();
        tick();
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            rootExited_ = true;
            metadata_.insert("error", "spawn_failed");
            tick();
        }
    });
    clock_.start();
    timer_.setInterval(100);
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    timer_.start();
    signal(SIGTERM, stopSignal);
    signal(SIGINT, stopSignal);
    signal(SIGPIPE, SIG_IGN); // A pre-exec gate peer may die before the release write.
    return true;
}

void NativeJobSupervisor::acceptConnection() {
    while (auto* socket = server_.nextPendingConnection()) {
        if (client_) {
            socket->disconnectFromServer();
            socket->deleteLater();
            continue;
        }
        client_ = socket;
        input_.clear();
        authenticated_ = false;
        client_->setReadBufferSize(256 * 1024 + 1);
        connect(client_, &QLocalSocket::readyRead, this, [this] { receive(); });
        connect(client_, &QLocalSocket::disconnected, this, [this, socket] {
            if (client_ == socket) {
                client_ = nullptr;
                authenticated_ = false;
                input_.clear();
            }
            socket->deleteLater();
        });
    }
}

void NativeJobSupervisor::receive() {
    input_.append(client_->readAll());
    if (input_.size() > 256 * 1024) {
        client_->disconnectFromServer();
        return;
    }
    for (auto end = input_.indexOf('\n'); end >= 0; end = input_.indexOf('\n')) {
        const auto message = QJsonDocument::fromJson(input_.left(end)).object();
        input_.remove(0, end + 1);
        command(message);
        if (!client_ || client_->state() != QLocalSocket::ConnectedState)
            return;
    }
}

void NativeJobSupervisor::command(const QJsonObject& message) {
    const auto operation = message.value("op").toString();
    if (!authenticated_) {
        if (operation != "auth" || message.value("lease_id").toString() != lease_) {
            client_->disconnectFromServer();
            return;
        }
        authenticated_ = true;
        lastPulse_ = clock_.elapsed();
        send("ready", metadata_);
    } else if (operation == "ping") {
        lastPulse_ = clock_.elapsed();
        send("pong");
    } else if (operation == "start" && !finalized_ && !stopping_) {
        launch(message);
    } else if (operation == "stop") {
        if (finalized_ && metadata_.value("state") == "needs_attention") {
            // Retry known identities without erasing uncertainty or replaying start.
            finalized_ = false;
            stopping_ = false;
        }
        const auto reason = message.value("reason").toString();
        requestStop(reason == "timed_out"     ? reason
                    : reason == "interrupted" ? reason
                                              : "cancelled");
    } else if (operation == "ack" && finalized_ && process_.state() == QProcess::NotRunning &&
               known_.isEmpty()) {
        QCoreApplication::quit();
    } else {
        send("error", {{"reason", "invalid_command"}});
    }
}

void NativeJobSupervisor::send(const QString& event, const QJsonObject& data) {
    if (client_ && authenticated_) {
        auto message = data;
        message.insert("event", event);
        client_->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
        client_->flush();
    }
}

bool NativeJobSupervisor::persist() {
    QJsonArray identities;
    for (const auto& identity : known_)
        identities.append(identity.json());
    metadata_.insert("known_processes", identities);
    QSaveFile file(directory_ + "/executor.json");
    if (!file.open(QIODevice::WriteOnly)) {
        cleanupError_ = true;
        return false;
    }
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    const auto bytes = QJsonDocument(metadata_).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        cleanupError_ = true;
        return false;
    }
    return true;
}

void NativeJobSupervisor::drainOutput() {
    while (process_.bytesAvailable() > 0) {
        const auto bytes = process_.read(64 * 1024);
        if (bytes.isEmpty())
            break;
        QString error;
        if (!metadata_.contains("output_error")) {
            if (!output_->append(taskId_, bytes, &error)) {
                metadata_.insert("output_error", "output_storage_failed");
                metadata_.insert("output_truncated", true);
            } else {
                const auto size = metadata_.value("output_size").toInteger() + bytes.size();
                metadata_.insert("output_size", size);
                if (size > BackgroundTaskOutput::maximumBytes)
                    metadata_.insert("output_truncated", true);
            }
        }
    }
}
} // namespace ketplus::daemon::executor
