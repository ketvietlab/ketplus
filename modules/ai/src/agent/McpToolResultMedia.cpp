#include "agent/McpToolResultMedia.h"

#include "agent/TranscriptNormalizer.h"

#include "DaemonClient.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace ketplus::mcp_media {
namespace {

constexpr qsizetype maximumSummaryCharacters = 160;

QString extensionFor(const QString& mimeType, const QByteArray& bytes) {
    if (mimeType == QStringLiteral("image/png") && bytes.startsWith("\x89PNG\r\n\x1a\n"))
        return QStringLiteral("png");
    if ((mimeType == QStringLiteral("image/jpeg") || mimeType == QStringLiteral("image/jpg")) &&
        bytes.startsWith("\xff\xd8\xff"))
        return QStringLiteral("jpg");
    return {};
}

void appendScalars(const QJsonObject& object, const QString& prefix, QStringList& parts) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        const QString key = it.key();
        if (key.endsWith(QStringLiteral("Id")) || key.endsWith(QStringLiteral("Ref")) ||
            TranscriptSanitizer::isSensitiveKey(key))
            continue;
        const QString name = prefix.isEmpty() ? key : prefix + QLatin1Char('.') + key;
        const QJsonValue value = it.value();
        if (value.isString())
            parts.append(name + QLatin1Char('=') + TranscriptSanitizer::text(value.toString()));
        else if (value.isDouble() || value.isBool())
            parts.append(name + QLatin1Char('=') + value.toVariant().toString());
        else if (value.isObject() && prefix.isEmpty())
            appendScalars(value.toObject(), name, parts);
    }
}

// {type:"image", media_ref:"sha256:<hex>", mime, bytes} written by the daemon media store.
QString resolveMediaRef(const QJsonObject& block, const QString& mediaDirectory) {
    const QString reference = block.value(QStringLiteral("media_ref")).toString();
    if (!reference.startsWith(QStringLiteral("sha256:")))
        return {};
    const QString hex = reference.mid(7);
    if (hex.size() != 64 || !std::all_of(hex.cbegin(), hex.cend(), [](const QChar character) {
            return (character >= QLatin1Char('0') && character <= QLatin1Char('9')) ||
                   (character >= QLatin1Char('a') && character <= QLatin1Char('f'));
        }))
        return {};
    const QString mime = block.value(QStringLiteral("mime")).toString();
    const QString extension = mime == QStringLiteral("image/png")    ? QStringLiteral("png")
                              : mime == QStringLiteral("image/jpeg") ? QStringLiteral("jpg")
                                                                     : QString();
    if (extension.isEmpty())
        return {};
    const QFileInfo file(QDir(mediaDirectory).filePath(hex + QLatin1Char('.') + extension));
    const qint64 expected = block.value(QStringLiteral("bytes")).toInteger(-1);
    if (!file.isFile() || file.isSymLink() || file.size() > maximumImageBytes ||
        (expected >= 0 && file.size() != expected))
        return {};
#if defined(Q_OS_UNIX)
    if (file.ownerId() != ::getuid())
        return {};
#endif
    return QUrl::fromLocalFile(file.absoluteFilePath()).toString();
}

} // namespace

QString directory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("transcript-media"));
}

QString daemonMediaDirectory() {
    // The daemon's McpMediaStore lives next to its journal (McpMediaStore::directoryForJournal).
    return QFileInfo(remote::DaemonClient::defaultStatePath())
        .absoluteDir()
        .filePath(QStringLiteral("media"));
}

QVariantList messageImages(const QJsonArray& blocks, const QString& mediaDirectory) {
    QVariantList images;
    for (const QJsonValue& value : blocks) {
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() != QStringLiteral("image"))
            continue;
        const QString url = resolveMediaRef(block, mediaDirectory);
        if (!url.isEmpty() && !images.contains(url))
            images.append(url);
    }
    return images;
}

QVariantList materializeImages(const QJsonObject& result, const QString& targetDirectory,
                               const QString& mediaDirectory) {
    QVariantList images;
    const QJsonArray content = result.value(QStringLiteral("content")).toArray();
    for (const QJsonValue& value : content) {
        if (images.size() >= maximumImagesPerCall)
            break;
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() != QStringLiteral("image"))
            continue;
        if (block.contains(QStringLiteral("media_ref"))) {
            QString url = resolveMediaRef(block, mediaDirectory);
            // Pruned or unreadable media keeps its slot so the gallery shows a placeholder.
            if (url.isEmpty())
                url = QLatin1String(unavailableSourcePrefix) + QString::number(images.size());
            if (!images.contains(url))
                images.append(url);
            continue;
        }
        const QString data = block.value(QStringLiteral("data")).toString();
        if (data.isEmpty() || data.size() > (maximumImageBytes / 3 + 1) * 4)
            continue;
        const auto decoded = QByteArray::fromBase64Encoding(
            data.toLatin1(), QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.isEmpty() || decoded.decoded.size() > maximumImageBytes)
            continue;
        const QString extension =
            extensionFor(block.value(QStringLiteral("mimeType")).toString(), decoded.decoded);
        if (extension.isEmpty())
            continue;
        const QDir target(targetDirectory);
        if (!target.mkpath(QStringLiteral(".")))
            continue;
        QFile::setPermissions(target.absolutePath(), QFileDevice::ReadOwner |
                                                         QFileDevice::WriteOwner |
                                                         QFileDevice::ExeOwner);
        const QString name =
            QString::fromLatin1(
                QCryptographicHash::hash(decoded.decoded, QCryptographicHash::Sha256).toHex()) +
            QLatin1Char('.') + extension;
        const QString path = target.absoluteFilePath(name);
        if (!QFileInfo::exists(path)) {
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly))
                continue;
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            if (file.write(decoded.decoded) != decoded.decoded.size() || !file.commit())
                continue;
        }
        const QString url = QUrl::fromLocalFile(path).toString();
        if (!images.contains(url))
            images.append(url);
    }
    return images;
}

QString argumentSummary(const QJsonValue& arguments) {
    QStringList parts;
    appendScalars(arguments.toObject(), {}, parts);
    QString summary = parts.join(QStringLiteral(" · "));
    if (summary.size() > maximumSummaryCharacters)
        summary = summary.left(maximumSummaryCharacters - 1) + QChar(0x2026);
    return summary;
}

} // namespace ketplus::mcp_media
