#include "remote/LinuxSshMachines.h"
#include "ssh/SshDaemonInstaller.h"
#include "ssh/SshMachineBackend.h"

namespace ketplus {
using remote::MachineBackend;

bool LinuxSshMachines::installationBusy(const QString& key) {
    if (!installers_.value(key) && !pending_.value(key).offered)
        return false;
    emit notice(tr("Finish or cancel daemon management in the open connection dialog first."),
                true);
    return true;
}
using remote::ssh::SshDaemonInstaller;

void LinuxSshMachines::offerInstall(const QString& key) {
    auto pending = pending_.find(key);
    auto* backend = backends_.value(key);
    auto* tunnel = backend ? backend->installationTunnel() : nullptr;
    if (!model_ || pending == pending_.end() || pending->offered || !tunnel)
        return;
    pending->offered = true;
    tunnel->holdForInstallation();
    emit model_->daemonInstallOffered(pending->host, releaseChannel_.configured());
}

void LinuxSshMachines::reviewInstall(const QString& host, const QString& action) {
    const QString key = pendingKey(host);
    const auto* backend = backends_.value(key);
    if (key.isEmpty() || installers_.value(key) || !backend || !pending_.value(key).offered ||
        !backend->installationTunnel())
        return;
    auto* installer = new SshDaemonInstaller(backend->installationTunnel(), releaseChannel_, this);
    installers_.insert(key, installer);
    connect(installer, &SshDaemonInstaller::consentRequested, this,
            [this, host](const QString& description) {
                if (model_)
                    emit model_->daemonConsentRequested(host, description);
            });
    connect(installer, &SshDaemonInstaller::progress, this, [this, host](const QString& message) {
        if (model_)
            emit model_->daemonInstallProgress(host, message);
    });
    connect(installer, &SshDaemonInstaller::connectionRequested, this, [this, key] {
        if (auto* machine = backends_.value(key))
            machine->connectToHost();
    });
    connect(installer, &SshDaemonInstaller::finished, this,
            [this, key](bool success, const QString& message) {
                finishInstall(key, success, message);
            });
    installer->review(action);
}

void LinuxSshMachines::finishInstall(const QString& key, bool success, const QString& message) {
    auto installer = installers_.take(key);
    if (installer)
        installer->deleteLater();
    auto pending = pending_.find(key);
    auto* backend = backends_.value(key);
    if (pending == pending_.end() || !backend || !model_)
        return;
    const auto attempt = *pending;
    pending_.erase(pending);
    if (!success)
        emit notice(message,
                    !installer || installer->state() != SshDaemonInstaller::State::Cancelled);
    if (success && backend->connectionState() == MachineBackend::ConnectionState::Connected) {
        publish();
        model_->reportSshConnected(attempt.host);
    } else if (success) {
        backend->disconnectFromHost();
        publish();
        emit model_->daemonOperationFinished(attempt.host, message);
    } else {
        // Removing the saved SSH target never removes remote installation/data. If rollback
        // lost SSH, its fixed error is surfaced rather than claiming the account is unchanged.
        backend->disconnectFromHost();
        if (attempt.added) {
            QString error;
            backend->removeMachine(&error);
            drop(key);
        } else {
            publish();
        }
        model_->reportSshFailure(attempt.host, LinuxMachine::Error::Other, message);
    }
}
} // namespace ketplus
