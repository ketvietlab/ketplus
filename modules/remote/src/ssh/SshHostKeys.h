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
// left out. `ssh -G` prints the paths unquoted, so a path with a space is rejoined when the
// joined path exists.
[[nodiscard]] QStringList knownHostsFiles(const QHash<QString, QString>& config);
// Lines that KnownHostsCommand captured: "<name> <type> <key>" per offered key.
[[nodiscard]] QList<HostKey> parseCaptured(const QByteArray& output);
// `ssh-keygen -F` output for the host, across the known_hosts files.
struct KnownEntries {
    // Plain host keys: key type -> keys.
    QHash<QString, QStringList> keys;
    // @revoked keys: never verified and never offered for acceptance.
    QStringList revoked;
    // A @cert-authority line names this host: ssh verifies its certificate itself.
    bool certificateAuthority{false};
    [[nodiscard]] bool isEmpty() const { return keys.isEmpty() && !certificateAuthority; }
    void merge(const KnownEntries& other);
};
[[nodiscard]] KnownEntries parseKnownEntries(const QByteArray& output);
// What ssh's stderr says about a failed call. Each pattern is anchored to a whole line, and
// every call runs with LogLevel=ERROR, so a server banner cannot imitate one.
enum class SshFailure { Other, AuthFailed, HostKeyChanged, NoMatchingHostKeyType };
[[nodiscard]] SshFailure classifySshError(const QByteArray& err);
// Host key algorithms that select a known key type, for a second probe.
[[nodiscard]] QStringList algorithmsFor(const QStringList& keyTypes);
// A value for a `-o Key=value` option: each path quoted, as ssh_config allows.
[[nodiscard]] QString quotedList(const QStringList& paths);
// Appends the key to the KetPlus known_hosts file (owner-only, directory created owner-only).
bool recordHostKey(const QString& file, const HostKey& key, QString* error = nullptr);
// The POSIX sh program run on the host to resolve the daemon socket and local token as the user
// unit does (docs/linux-headless-daemon.md). It prints "ketplus-socket <present|missing> <path>"
// and, for a present socket, "ketplus-token <base64>" when the token file is owner-only,
// "ketplus-token-permissive" when it is not, or "ketplus-token-missing".
[[nodiscard]] QString remoteSocketProbe();
struct DaemonProbe {
    enum class Token { Missing, Permissive, Present };
    bool present{false};
    QString socketPath;
    Token token{Token::Missing};
    // The token file's bytes; held in memory only.
    QByteArray credential;
};
// Parses the probe output; false when the socket line is absent or names an unsafe path.
bool parseDaemonProbe(const QByteArray& output, DaemonProbe* probe);

} // namespace ketplus::remote::ssh
