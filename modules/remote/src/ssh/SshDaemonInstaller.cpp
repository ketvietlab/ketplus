#include "ssh/SshDaemonInstaller.h"
#include "ssh/SshProcess.h"
#include "ssh/SshTunnel.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

static void initializeInstallerResource() { Q_INIT_RESOURCE(DaemonInstaller); }
namespace ketplus::remote::ssh {
SshDaemonInstaller::SshDaemonInstaller(SshTunnel* tunnel, DaemonReleaseChannel channel,
                                       QObject* parent)
    : QObject(parent), tunnel_(tunnel), channel_(std::move(channel)),
      network_(new QNetworkAccessManager(this)),
      work_(std::make_unique<QTemporaryDir>(QDir::tempPath() + "/ketplus-install-XXXXXX")) {
    initializeInstallerResource();
    if (work_->isValid())
        QFile::setPermissions(work_->path(),
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}
SshDaemonInstaller::~SshDaemonInstaller() {
    if (process_)
        process_->closeWriteChannel(); // EOF makes the remote transaction roll back.
}
void SshDaemonInstaller::setState(State state, const QString& message) {
    state_ = state;
    emit progress(message);
}
bool SshDaemonInstaller::identityMatches(bool reconnect) const {
    return tunnel_ && tunnel_->hostKeyStatus() == HostKeyStatus::Verified &&
           tunnel_->fingerprint() == fingerprint_ &&
           (reconnect || tunnel_->connectionGeneration() == sshGeneration_);
}
void SshDaemonInstaller::complete(bool success, const QString& message) {
    if (terminal_)
        return;
    terminal_ = true;
    ++generation_;
    if (reply_)
        reply_->abort();
    state_ = success                                        ? State::Succeeded
             : cancelled_ && (!transaction_ || rolledBack_) ? State::Cancelled
                                                            : State::Failed;
    emit finished(success, message);
}
void SshDaemonInstaller::fail(const QString& message) {
    failureMessage_ = message;
    if (process_ && process_->state() != QProcess::NotRunning) {
        setState(transaction_ ? State::RollingBack : State::Preflight,
                 transaction_ ? message + tr(" Rolling back…") : message);
        process_->closeWriteChannel();
        return;
    }
    complete(false, message);
}
void SshDaemonInstaller::review(const QString& action) {
    if (state_ != State::Idle)
        return;
    if (action != "install" && action != "start" && action != "uninstall") {
        fail(tr("Unknown daemon operation."));
        return;
    }
    if (!work_->isValid() || !tunnel_ || tunnel_->hostKeyStatus() != HostKeyStatus::Verified ||
        (tunnel_->state() != SshTunnel::State::DaemonSocketMissing &&
         tunnel_->state() != SshTunnel::State::Connected)) {
        fail(tr("Verify this SSH host before managing its daemon."));
        return;
    }
    if (action == "install" && !channel_.configured()) {
        fail(tr("Install unavailable: no trusted release channel."));
        return;
    }
    action_ = action;
    fingerprint_ = tunnel_->fingerprint();
    sshGeneration_ = tunnel_->connectionGeneration();
    socket_ = tunnel_->remoteSocketPath();
    setState(State::Preflight, tr("Checking the Linux account without changing it…"));
    command({{"action", "preflight"}, {"socket", socket_}}, false);
}
void SshDaemonInstaller::command(const QJsonObject& description, bool transaction) {
    if (terminal_ || cancelled_)
        return;
    if (!identityMatches()) {
        fail(tr("The verified SSH identity changed. Review the operation again."));
        return;
    }
    QFile script(":/ketplus/ssh-install/transaction.py");
    if (!script.open(QIODevice::ReadOnly)) {
        fail(tr("The installer resource is missing."));
        return;
    }
    QString source = QString::fromUtf8(script.readAll());
    source.replace('\'', "'\"'\"'");
    const QString program =
        "exec python3 -c '" + source + "' '" +
        QString::fromLatin1(QJsonDocument(description).toJson(QJsonDocument::Compact).toBase64()) +
        "'";
    const auto arguments = tunnel_->installationArguments(program, work_->filePath("known_hosts"));
    if (arguments.isEmpty()) {
        fail(tr("The verified SSH install channel is unavailable."));
        return;
    }
    auto* process = new QProcess(this);
    process_ = process;
    transaction_ = transaction;
    output_.clear();
    connect(process, &QProcess::readyReadStandardOutput, this, &SshDaemonInstaller::readProcess);
    connect(process, &QProcess::readyReadStandardError, process, [process] {
        process->readAllStandardError(); // Never surface account data or server banners.
    });
    connect(process, &QProcess::bytesWritten, this, &SshDaemonInstaller::pumpArchive);
    connect(process, &QProcess::started, this, [this, process, transaction] {
        if (transaction)
            pumpArchive();
        else
            process->closeWriteChannel();
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            complete(false, tr("Unable to start the verified SSH installer."));
    });
    connect(
        process, &QProcess::finished, this,
        [this, process, transaction](int code, QProcess::ExitStatus exit) {
            readProcess();
            process->deleteLater();
            if (process_ == process)
                process_.clear();
            if (terminal_)
                return;
            if (cancelled_) {
                complete(
                    false,
                    rolledBack_ || !transaction
                        ? tr("Daemon operation cancelled; no installation changes were retained.")
                        : tr("Daemon operation cancelled. Rollback could not be confirmed; check "
                             "the host before retrying."));
                return;
            }
            if (transaction && code == 0 && exit == QProcess::NormalExit &&
                process->property("committed").toBool()) {
                complete(true, action_ == "uninstall"
                                   ? tr("Daemon uninstalled. User data was retained.")
                                   : tr("Daemon installed and connected over SSH."));
                return;
            }
            if (transaction || code != 0 || preflight_.isEmpty()) {
                complete(false, failureMessage_.isEmpty()
                                    ? tr("Daemon operation failed or was rolled back. Check the "
                                         "host if SSH was lost during rollback.")
                                    : failureMessage_);
                return;
            }
            if (action_ == "install") {
                if (preflight_.value("installed").toBool())
                    action_ = "upgrade";
                setState(State::Manifest, tr("Fetching the signed release manifest…"));
                fetch(channel_.manifestUrl, daemonManifestLimit, [this](QByteArray bytes) {
                    manifest_ = std::move(bytes);
                    QUrl signature = channel_.manifestUrl;
                    signature.setPath(signature.path() + ".sig");
                    fetch(signature, 64, [this](QByteArray signedBytes) {
                        QString error;
                        artifact_ =
                            verifyDaemonRelease(channel_, manifest_, signedBytes,
                                                preflight_.value("arch").toString(), &error);
                        if (!artifact_) {
                            fail(error);
                            return;
                        }
                        describe();
                    });
                });
            } else {
                if (!preflight_.value("installed").toBool()) {
                    fail(tr("There is no receipt for a daemon installed by ketPlus. Nothing "
                            "was changed."));
                    return;
                }
                describe();
            }
        });
    process->setProcessEnvironment(sshEnvironment());
    process->start(tunnel_->sshProgram(), arguments);
    QTimer::singleShot(300000, process, [this, process] {
        if (process->state() == QProcess::NotRunning)
            return;
        process->closeWriteChannel();
        fail(tr("The daemon installation timed out."));
    });
}
void SshDaemonInstaller::readProcess() {
    if (!process_)
        return;
    output_ += process_->readAllStandardOutput();
    if (output_.size() > daemonManifestLimit) {
        fail(tr("The SSH installer returned too much output."));
        return;
    }
    while (output_.contains('\n')) {
        const auto end = output_.indexOf('\n');
        const auto value = QJsonDocument::fromJson(output_.left(end)).object();
        output_.remove(0, end + 1);
        if (value.contains("error")) {
            // Fixed protocol errors originate from the signed-in account. Still plain text,
            // never markup, a shell command, a link or a trust instruction.
            failureMessage_ = value.value("error").toString().left(512);
            process_->closeWriteChannel();
            continue; // Parse a following rollback confirmation before publishing the result.
        }
        if (!transaction_) {
            preflight_ = value;
        } else if (value.value("phase") == "ready") {
            if (cancelled_ || terminal_ || state_ == State::RollingBack) {
                process_->closeWriteChannel();
                continue;
            }
            if (action_ == "uninstall") {
                commitSent_ = true;
                process_->write("commit\n");
                process_->closeWriteChannel();
            } else {
                setState(State::Connecting, tr("Verifying the daemon connection…"));
                if (!cancelled_ && !terminal_)
                    emit connectionRequested();
            }
        } else if (value.value("phase") == "committed") {
            // Wait for process exit too: cleanup/commit must have completed successfully.
            process_->setProperty("committed", true);
        } else if (value.value("phase") == "rolled_back") {
            rolledBack_ = true;
        }
    }
}
void SshDaemonInstaller::describe() {
    setState(State::Consent, tr("Review the daemon operation."));
    if (terminal_ || cancelled_)
        return;
    const QString release = artifact_ ? artifact_->version : preflight_.value("version").toString();
    const QString size = artifact_ ? tr("\nDownload: %1 MiB. Signed manifest verified; archive "
                                        "SHA-256 is checked before installation.")
                                         .arg(double(artifact_->bytes) / (1024 * 1024), 0, 'f', 1)
                                   : QString();
    const QString verb = action_ == "install"   ? tr("Install")
                         : action_ == "upgrade" ? tr("Upgrade")
                         : action_ == "start"   ? tr("Start")
                                                : tr("Uninstall");
    const QString behavior =
        action_ == "uninstall"
            ? tr("Stops only an idle managed daemon and removes its active runtime. User data and "
                 "credentials remain.")
            : tr("Starts a user process for SSH; no login service or sudo. An upgrade stops only "
                 "an idle managed daemon. Rollback restores the previous runtime and data.");
    emit consentRequested(tr("%1 ketPlus daemon %2 on %3\nSSH host key: %4\nArchitecture: %5%6\n"
                             "Location: ~/.local/opt/ketplus (this account only).\n%7")
                              .arg(verb, release, tunnel_ ? tunnel_->targetAlias() : QString(),
                                   fingerprint_, preflight_.value("arch").toString(), size,
                                   behavior));
}
void SshDaemonInstaller::confirm(bool accepted) {
    if (state_ != State::Consent)
        return;
    if (!accepted) {
        cancel();
        return;
    }
    if (!identityMatches()) {
        fail(tr("The SSH identity changed after review."));
        return;
    }
    const auto activate = [this] {
        QJsonObject request{{"action", action_},
                            {"socket", socket_},
                            {"profile", preflight_.value("profile")},
                            {"installation", preflight_.value("installation")}};
        if (artifact_)
            request.insert("artifact", artifact_->remoteDescription());
        setState(State::Installing, tr("Installing over the verified SSH connection…"));
        command(request, true);
    };
    if (artifact_) {
        setState(State::Download, tr("Downloading and checking the complete Linux runtime…"));
        fetch(
            artifact_->url, artifact_->bytes,
            [this, activate](QByteArray hash) {
                archive_ = std::make_unique<QFile>(work_->filePath("runtime.tar.gz"));
                if (!archive_->open(QIODevice::ReadOnly) || archive_->size() != artifact_->bytes) {
                    fail(tr("The release archive has the wrong size."));
                    return;
                }
                if (hash.toHex() != artifact_->sha256) {
                    fail(tr("The release archive checksum differs. Nothing was installed."));
                    return;
                }
                archive_->seek(0);
                activate();
            },
            true);
    } else {
        activate();
    }
}
void SshDaemonInstaller::pumpArchive() {
    if (!process_ || !transaction_ || !archive_ || cancelled_ ||
        process_->state() != QProcess::Running)
        return;
    while (!archive_->atEnd() && process_->bytesToWrite() < 256 * 1024) {
        const auto bytes = archive_->read(65536);
        if (bytes.isEmpty() || process_->write(bytes) != bytes.size()) {
            fail(tr("Unable to transfer the verified runtime archive."));
            return;
        }
    }
}
void SshDaemonInstaller::connectionResult(bool connected) {
    if (state_ != State::Connecting || !process_ || cancelled_ || terminal_)
        return;
    if (!connected || !identityMatches(true)) {
        fail(tr("The daemon connection did not verify. Restoring the previous installation."));
        return;
    }
    commitSent_ = true;
    process_->write("commit\n");
    process_->closeWriteChannel();
}
void SshDaemonInstaller::cancel() {
    if (terminal_ || commitSent_)
        return;
    cancelled_ = true;
    ++generation_;
    if (reply_)
        reply_->abort();
    if (process_ && process_->state() != QProcess::NotRunning) {
        setState(State::RollingBack, tr("Cancelling and restoring the previous installation…"));
        process_->closeWriteChannel();
    } else {
        complete(false, tr("Cancelled. Nothing was installed."));
    }
}
} // namespace ketplus::remote::ssh
