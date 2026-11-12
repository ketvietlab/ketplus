#pragma once

#include "ssh/SshTunnel.h"

#include <QHash>
#include <QList>
#include <QString>

// The Linux machines Desktop reaches over SSH, kept across restarts (docs/ssh-transport.md,
// ADR 0119 "Machine identity" and "Host key rotation"). It holds no secret: per machine the SSH
// target (alias, user, port) and the daemon host key fingerprint last trusted as its machine id,
// plus the conversations bound to each machine id. Accepted SSH host keys stay in SshTunnel's
// KetPlus known_hosts file.
namespace ketplus::remote::ssh {

// Splits Connections' "user@host[:port]" (or "[v6]:port") input into an SshTarget. Returns an
// invalid target and sets `error` when the input is not one destination.
[[nodiscard]] SshTarget parseSshDestination(const QString& input, QString* error = nullptr);
// The stable key a target is stored under: "user@alias:port" with empty parts omitted.
[[nodiscard]] QString sshTargetKey(const SshTarget& target);

struct SshMachineRecord {
    SshTarget target;
    // The daemon's host key fingerprint, or empty until its first hello.
    QString machineId;
};

class SshMachineStore final {
  public:
    // <GenericDataLocation>/Ketsuite/KetPlus/ssh/machines.json, beside the KetPlus known_hosts.
    [[nodiscard]] static QString defaultPath();

    explicit SshMachineStore(QString path = defaultPath());

    // False when the file exists but cannot be read as a store; it is then never overwritten.
    [[nodiscard]] bool isUsable() const { return usable_; }
    [[nodiscard]] QString path() const { return path_; }

    [[nodiscard]] QList<SshMachineRecord> machines() const { return machines_; }
    [[nodiscard]] bool contains(const QString& targetKey) const;
    [[nodiscard]] QString machineId(const QString& targetKey) const;

    // Adds a target with no machine id yet; adding a known target keeps its record.
    bool add(const SshTarget& target, QString* error = nullptr);
    // Forgets the target, its machine id and the conversations bound to that id.
    bool remove(const QString& targetKey, QString* error = nullptr);
    // The first hello: records the machine id of a target that has none.
    bool trustFirstIdentity(const QString& targetKey, const QString& machineId,
                            QString* error = nullptr);
    // The explicit "Trust the new identity": replaces the id and moves every conversation bound
    // to the old id to the new one, in one write.
    bool retrust(const QString& targetKey, const QString& machineId, QString* error = nullptr);

    bool bindConversation(const QString& conversationId, const QString& machineId,
                          QString* error = nullptr);
    [[nodiscard]] QString conversationMachine(const QString& conversationId) const;

  private:
    void load();
    bool save(QString* error);
    [[nodiscard]] qsizetype indexOf(const QString& targetKey) const;

    QString path_;
    bool usable_{true};
    QList<SshMachineRecord> machines_;
    QHash<QString, QString> bindings_;
};

} // namespace ketplus::remote::ssh
