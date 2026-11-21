#include "ssh/SshTunnel.h"
#include <QFile>
#include <QTimer>

namespace ketplus::remote::ssh {
void SshTunnel::holdForInstallation() {
    installationHeld_ = true;
    retry_->stop();
}
QStringList SshTunnel::installationArguments(const QString& command, const QString& trustFile) {
    if (hostKeyStatus_ != HostKeyStatus::Verified || !work_ ||
        (state_ != State::Connected && state_ != State::DaemonSocketMissing))
        return {};
    if (!QFile::exists(trustFile)) {
        QByteArray contents;
        if (certificate_) {
            // Preserve CA/revocation entries, but freeze the files for the whole transaction.
            auto files = knownFiles_;
            files.append(options_.knownHostsFile);
            for (const auto& path : files) {
                QFile file(path);
                if (file.open(QIODevice::ReadOnly)) {
                    if (file.size() > 1024 * 1024)
                        return {};
                    contents += file.readAll() + '\n';
                }
            }
        } else {
            contents =
                (hostKey_.knownHostsName + ' ' + hostKey_.type + ' ' + hostKey_.base64 + '\n')
                    .toUtf8();
        }
        QFile file(trustFile);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
            file.write(contents) != contents.size())
            return {};
    }
    holdForInstallation();
    auto arguments = verifiedOptions(true);
    for (auto& argument : arguments)
        if (argument.startsWith("UserKnownHostsFile="))
            argument = "UserKnownHostsFile=\"" + trustFile + "\"";
    arguments << target_.alias << command;
    return arguments;
}
} // namespace ketplus::remote::ssh
