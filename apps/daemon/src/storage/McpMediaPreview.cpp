#include "storage/McpMediaStore.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QRegularExpression>

namespace ketplus::daemon {
namespace {
QJsonObject unavailable() { return {{"ok", false}, {"error_code", "media_unavailable"}}; }
} // namespace

QJsonObject McpMediaStore::encodePreview(QIODevice* source, int maximumSide) {
    QImageReader reader(source);
    const QSize original = reader.size();
    if (!original.isValid() || original.width() > 16384 || original.height() > 16384 ||
        qint64(original.width()) * original.height() > 64 * 1024 * 1024)
        return unavailable();
    if (original.width() > maximumSide || original.height() > maximumSide)
        reader.setScaledSize(original.scaled(maximumSide, maximumSide, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull())
        return unavailable();
    // JPEG has no alpha: a transparent screenshot or icon goes onto white, not black.
    if (image.hasAlphaChannel()) {
        QImage flattened(image.size(), QImage::Format_RGB32);
        flattened.fill(Qt::white);
        QPainter(&flattened).drawImage(0, 0, image);
        image = flattened;
    }
    QByteArray data;
    for (int attempt = 0; attempt < 5; ++attempt) {
        data.clear();
        QBuffer buffer(&data);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPEG", 78))
            return unavailable();
        if (data.size() <= 192 * 1024)
            break;
        image = image.scaled(image.size() * 0.75, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (data.size() > 192 * 1024)
        return unavailable();
    return {{"ok", true},
            {"mime", "image/jpeg"},
            {"width", image.width()},
            {"height", image.height()},
            {"data", QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding |
                                                       QByteArray::OmitTrailingEquals))}};
}

QJsonObject McpMediaStore::preview(const QString& sessionId, const QString& reference,
                                   int maximumSide) const {
    static const QRegularExpression valid("^sha256:([a-f0-9]{64})$");
    const auto matched = valid.match(reference);
    if (!matched.hasMatch())
        return unavailable();
    const QString hash = matched.captured(1);
    QFile refs(refsPath(sessionId));
    if (!refs.open(QIODevice::ReadOnly) || !refs.readAll().split('\n').contains(hash.toLatin1()))
        return unavailable();
    for (const QString& suffix : {QStringLiteral(".png"), QStringLiteral(".jpg")}) {
        QFile file(QDir(directory_).filePath(hash + suffix));
        const QFileInfo info(file);
        if (!info.isFile() || info.isSymLink() || info.size() > maximumImageBytes)
            continue;
        if (!file.open(QIODevice::ReadOnly))
            return unavailable();
        QJsonObject result = encodePreview(&file, maximumSide);
        if (result.value("ok").toBool())
            result.insert("media_ref", reference);
        return result;
    }
    return unavailable();
}
} // namespace ketplus::daemon
