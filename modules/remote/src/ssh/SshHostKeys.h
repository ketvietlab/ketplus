#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

// Host-key helpers for SshTunnel: OpenSSH output parsing, fingerprints and the KetPlus-owned
// known_hosts file. Nothing here starts a process.
namespace ketplus::remote::ssh {

struct HostKey {
    // The name ssh looks the key up by (%H): "host" or "[host]:port", or the HostKeyAlias.
    QString knownHostsName;
    QString type;
    QString base64;
    [[nodiscard]] bool isValid() const { return !type.isEmpty() && !base64.isEmpty(); }
    // "SHA256:<base64 without padding>", as `ssh-keygen -l` prints it.
    [[nodiscard]] QString fingerprint() const;
};

// `ssh -G` output: lower-case keyword -> value (the first value of a repeated keyword).
[[nodiscard]] QHash<QString, QString> parseResolvedConfig(const QByteArray& output);
// The known_hosts files a resolved config names, with "~" expanded; "none" and "/dev/null" are
// left out.
[[nodiscard]] QStringList knownHostsFiles(const QHash<QString, QString>& config);
// Lines that KnownHostsCommand captured: "<name> <type> <key>" per offered key.
[[nodiscard]] QList<HostKey> parseCaptured(const QByteArray& output);
// `ssh-keygen -F` output for one file: the entries' key type -> keys.
[[nodiscard]] QHash<QString, QStringList> parseKnownEntries(const QByteArray& output);
// Host key algorithms that select a known key type, for a second probe.
[[nodiscard]] QStringList algorithmsFor(const QStringList& keyTypes);
// A value for a `-o Key=value` option: each path quoted, as ssh_config allows.
[[nodiscard]] QString quotedList(const QStringList& paths);
// Appends the key to the KetPlus known_hosts file (owner-only, directory created owner-only).
bool recordHostKey(const QString& file, const HostKey& key, QString* error = nullptr);
// The POSIX sh program run on the host to resolve the daemon socket as the user unit does
// (docs/linux-headless-daemon.md). It prints "ketplus-socket <present|missing> <path>".
[[nodiscard]] QString remoteSocketProbe();
// Parses that line; false when it is absent or names a path unsafe for `-L`.
bool parseSocketProbe(const QByteArray& output, bool* present, QString* path);

} // namespace ketplus::remote::ssh
