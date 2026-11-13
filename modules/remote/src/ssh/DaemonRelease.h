#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <optional>

namespace ketplus::remote::ssh {

// ADR 0124. The release owner supplies the public key at build time. An empty key is
// deliberately not replaced with a network-provided key or an unsigned checksum.
struct DaemonReleaseChannel {
    QUrl manifestUrl;
    QByteArray publicKey;
    QString version;
    QString controlUrl;
    bool loopbackQA{false}; // Constructor injection for tests, never an environment override.
    [[nodiscard]] static DaemonReleaseChannel production();
    [[nodiscard]] bool configured() const;
    [[nodiscard]] bool permits(const QUrl& url) const;
};

struct DaemonReleaseArtifact {
    QString version;
    QString revision;
    QString architecture;
    QString controlUrl;
    QUrl url;
    qint64 bytes{0};
    QByteArray sha256;
    [[nodiscard]] QJsonObject remoteDescription() const;
};

// The signature is exactly 64 raw Ed25519 bytes, over the exact manifest bytes (no
// JSON reserialization). Select only after signature verification. Limits also apply
// to correctly signed manifests, to prevent accidental unbounded allocations.
[[nodiscard]] std::optional<DaemonReleaseArtifact>
verifyDaemonRelease(const DaemonReleaseChannel& channel, const QByteArray& manifest,
                    const QByteArray& signature, const QString& architecture, QString* error);
inline constexpr qint64 daemonArchiveLimit = 512 * 1024 * 1024;
inline constexpr qint64 daemonManifestLimit = 64 * 1024;

} // namespace ketplus::remote::ssh
