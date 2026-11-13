#pragma once

#include "remote/LinuxMachineModel.h"
#include "ssh/DaemonRelease.h"
#include "ssh/SshMachineStore.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <functional>
#include <memory>

namespace ketplus::remote::ssh {
class SshLink;
class SshMachineBackend;
class SshDaemonInstaller;
} // namespace ketplus::remote::ssh

namespace ketplus {

// The SSH half of Connections (ADR 0119, docs/ssh-transport.md). Every machine saved in
// SshMachineStore has one SshMachineBackend, and its live state, route, verified host key and
// errors are written into the LinuxMachineModel under the daemon's host key fingerprint, the
// ADR 0119 machine id. It carries out the model's intents for SSH machines: the dialog's
// connect flow (with the first-use host key), Connect, Disconnect, Remove, forgetting a
// mismatched key ketPlus accepted, and trusting a changed daemon identity. Relay machines'
// intents are left to the Relay backend.
class LinuxSshMachines final : public QObject {
    Q_OBJECT

  public:
    using LinkFactory =
        std::function<std::unique_ptr<remote::ssh::SshLink>(const remote::ssh::SshTarget&)>;
    // Links over the system OpenSSH client with the KetPlus known_hosts file.
    [[nodiscard]] static LinkFactory systemLinks();

    LinuxSshMachines(LinuxMachineModel* model,
                     QString storePath = remote::ssh::SshMachineStore::defaultPath(),
                     LinkFactory links = systemLinks(), QObject* parent = nullptr,
                     remote::ssh::DaemonReleaseChannel channel =
                         remote::ssh::DaemonReleaseChannel::production());
    ~LinuxSshMachines() override;

    // Opens every saved machine whose identity is trusted, as Connections opens.
    void connectSaved();
    // The backend of the SSH machine with this ADR 0119 machine id; null for any other id.
    [[nodiscard]] remote::ssh::SshMachineBackend* backend(const QString& machineId) const;
    // backend(), only while the row shows the SSH route.
    [[nodiscard]] remote::ssh::SshMachineBackend* shownBackend(const QString& machineId) const;
    [[nodiscard]] const remote::ssh::SshMachineStore& store() const { return store_; }

  signals:
    // Something to tell the user outside the connect card (a failed removal, a key in the
    // user's own known_hosts).
    void notice(const QString& message, bool error);

  private:
    // A connect started from the dialog for `host`, as the user typed it.
    struct Pending {
        QString host;
        // The dialog added this target: a failed or cancelled attempt removes it again.
        bool added{false};
        QString fingerprint;
        std::function<void(bool)> decide;
        bool manage{false};
        bool offered{false};
    };

    remote::ssh::SshMachineBackend* ensure(const remote::ssh::SshTarget& target);
    void drop(const QString& targetKey);
    void changed(const QString& targetKey);
    void publish();
    [[nodiscard]] QString keyFor(const QString& machineId) const;
    [[nodiscard]] QString pendingKey(const QString& host) const;
    [[nodiscard]] LinuxMachine machineFor(const remote::ssh::SshMachineBackend& backend) const;

    void startConnect(const QString& host);
    void decideHostKey(const QString& host, const QString& fingerprint, bool accepted);
    void cancelConnect(const QString& host);
    void remove(const QString& machineId);
    void forgetHostKey(const QString& machineId);
    void trustIdentity(const QString& machineId);
    void offerInstall(const QString& key);
    void reviewInstall(const QString& host, const QString& action);
    void finishInstall(const QString& key, bool success, const QString& message);

    QPointer<LinuxMachineModel> model_;
    remote::ssh::SshMachineStore store_;
    LinkFactory links_;
    // Target key -> backend, in the store's order.
    QStringList order_;
    QHash<QString, remote::ssh::SshMachineBackend*> backends_;
    QHash<QString, Pending> pending_;
    // Machine ids the user disconnected: connectSaved leaves them closed until they connect.
    QSet<QString> closedByUser_;
    remote::ssh::DaemonReleaseChannel releaseChannel_;
    QHash<QString, QPointer<remote::ssh::SshDaemonInstaller>> installers_;
};

} // namespace ketplus
