#include "ssh/DaemonRelease.h"
#include "KetPlusProductVersion.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <openssl/evp.h>

namespace ketplus::remote::ssh {

DaemonReleaseChannel DaemonReleaseChannel::production() {
    return {QUrl(QStringLiteral(KETPLUS_DAEMON_RELEASE_MANIFEST_URL)),
            QByteArray::fromHex(QByteArray(KETPLUS_DAEMON_RELEASE_PUBLIC_KEY)),
            QStringLiteral(KETPLUS_PRODUCT_VERSION), QStringLiteral(KETPLUS_DEFAULT_CONTROL_URL),
            false};
}

bool DaemonReleaseChannel::permits(const QUrl& url) const {
    if (!url.isValid() || !url.userInfo().isEmpty() || url.hasFragment() || url.isRelative() ||
        url.host().isEmpty())
        return false;
    const auto segments = url.path(QUrl::FullyDecoded).split('/');
    if (segments.contains(".") || segments.contains(".."))
        return false;
    if (loopbackQA)
        return url.scheme() == "http" && url.host() == "127.0.0.1" &&
               url.port() == manifestUrl.port();
    if (url.scheme() != "https" || (url.port() != -1 && url.port() != 443))
        return false;
    return (url.host() == "github.com" &&
            url.path().startsWith("/ketvietlab/ketplus/releases/download/v" + version + "/")) ||
           url.host() == "release-assets.githubusercontent.com" ||
           url.host() == "objects.githubusercontent.com";
}

bool DaemonReleaseChannel::configured() const {
    return publicKey.size() == 32 && !version.isEmpty() && !controlUrl.isEmpty() &&
           permits(manifestUrl) && (loopbackQA || manifestUrl.host() == "github.com");
}

QJsonObject DaemonReleaseArtifact::remoteDescription() const {
    return {{"version", version},
            {"revision", revision},
            {"architecture", architecture},
            {"control_url", controlUrl},
            {"bytes", bytes},
            {"sha256", QString::fromLatin1(sha256)}};
}

std::optional<DaemonReleaseArtifact>
verifyDaemonRelease(const DaemonReleaseChannel& channel, const QByteArray& manifest,
                    const QByteArray& signature, const QString& architecture, QString* error) {
    const auto reject = [error](const char* reason) -> std::optional<DaemonReleaseArtifact> {
        if (error)
            *error = QString::fromLatin1(reason);
        return std::nullopt;
    };
    if (!channel.configured())
        return reject("Install unavailable: no trusted release channel.");
    if (manifest.isEmpty() || manifest.size() > daemonManifestLimit || signature.size() != 64)
        return reject("The release manifest or signature has an invalid size.");
    auto* key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(channel.publicKey.constData()), 32);
    auto* context = EVP_MD_CTX_new();
    const bool verified =
        key && context && EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestVerify(context, reinterpret_cast<const unsigned char*>(signature.constData()),
                         size_t(signature.size()),
                         reinterpret_cast<const unsigned char*>(manifest.constData()),
                         size_t(manifest.size())) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    if (!verified)
        return reject("The release signature is not trusted. Nothing was installed.");
    const auto document = QJsonDocument::fromJson(manifest);
    if (!document.isObject())
        return reject("The signed release manifest is not a JSON object.");
    const auto root = document.object();
    static const QRegularExpression revisionPattern("^[0-9a-f]{12,40}$");
    if (root.value("schema").toInt() != 1 || root.value("product") != "ketplus-daemon" ||
        root.value("version").toString() != channel.version ||
        root.value("tag").toString() != "v" + channel.version ||
        !revisionPattern.match(root.value("revision").toString()).hasMatch() ||
        root.value("control_url").toString() != channel.controlUrl ||
        !root.value("artifacts").isArray() || root.value("artifacts").toArray().size() > 8)
        return reject("The release does not match this desktop version or control environment.");
    if (architecture != "x86_64" && architecture != "aarch64")
        return reject("This Linux architecture is not supported.");
    std::optional<DaemonReleaseArtifact> found;
    static const QRegularExpression hashPattern("^[0-9a-f]{64}$");
    for (const auto& value : root.value("artifacts").toArray()) {
        const auto item = value.toObject();
        if (item.value("os") != "linux" || item.value("arch").toString() != architecture)
            continue;
        DaemonReleaseArtifact artifact{channel.version,
                                       root.value("revision").toString(),
                                       architecture,
                                       channel.controlUrl,
                                       QUrl(item.value("url").toString()),
                                       item.value("bytes").toInteger(-1),
                                       item.value("sha256").toString().toLatin1()};
        if (found || !channel.permits(artifact.url) || artifact.bytes <= 0 ||
            artifact.bytes > daemonArchiveLimit ||
            !hashPattern.match(QString::fromLatin1(artifact.sha256)).hasMatch() ||
            item.value("layout") != "ketplus-runtime-v1")
            return reject("The signed Linux artifact descriptor is invalid or ambiguous.");
        found = artifact;
    }
    return found ? found : reject("No matching Linux daemon artifact is available.");
}
} // namespace ketplus::remote::ssh
