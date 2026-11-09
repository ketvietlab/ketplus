#include "storage/McpMediaStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QSize>
#include <QtEndian>

#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ketplus::daemon {
namespace {

constexpr QFileDevice::Permissions ownerDirectory =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
constexpr QFileDevice::Permissions ownerFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

QString extensionFor(const QString& mimeType, const QByteArray& bytes) {
    if (mimeType == QStringLiteral("image/png") && bytes.startsWith("\x89PNG\r\n\x1a\n"))
        return QStringLiteral("png");
    if ((mimeType == QStringLiteral("image/jpeg") || mimeType == QStringLiteral("image/jpg")) &&
        bytes.startsWith("\xff\xd8\xff"))
        return QStringLiteral("jpg");
    return {};
}

quint32 bigEndian32(const QByteArray& bytes, const qsizetype offset) {
    return qFromBigEndian<quint32>(bytes.constData() + offset);
}

quint16 bigEndian16(const QByteArray& bytes, const qsizetype offset) {
    return qFromBigEndian<quint16>(bytes.constData() + offset);
}

// Header-only dimension parsing; no QtGui in the daemon.
QSize imageSize(const QString& extension, const QByteArray& bytes) {
    if (extension == QStringLiteral("png")) {
        if (bytes.size() >= 24 && bytes.mid(12, 4) == "IHDR")
            return {static_cast<int>(bigEndian32(bytes, 16)),
                    static_cast<int>(bigEndian32(bytes, 20))};
        return {};
    }
    qsizetype offset = 2;
    while (offset + 9 < bytes.size()) {
        if (static_cast<uchar>(bytes.at(offset)) != 0xFF)
            return {};
        const uchar marker = static_cast<uchar>(bytes.at(offset + 1));
        if (marker == 0xFF) {
            ++offset;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
            offset += 2;
            continue;
        }
        const bool startOfFrame =
            marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (startOfFrame)
            return {bigEndian16(bytes, offset + 7), bigEndian16(bytes, offset + 5)};
        offset += 2 + bigEndian16(bytes, offset + 2);
    }
    return {};
}

// Opens `path` only if what is opened is a regular file. The check is made on the opened
// descriptor, without blocking: a path swapped for a FIFO after a stat must not hang the daemon.
bool openRegularFile(const QString& path, QFile* file, qint64* size) {
#ifdef Q_OS_UNIX
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat status{};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
        !file->open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return false;
    }
    *size = status.st_size;
    return true;
#else
    const QFileInfo info(path);
    *size = info.size();
    return info.isFile() && file->open(QIODevice::ReadOnly);
#endif
}

QJsonObject omitted(const QString& type, const QString& reason) {
    return {{QStringLiteral("type"), type},
            {QStringLiteral("omitted"), true},
            {QStringLiteral("reason"), reason}};
}

} // namespace

bool McpMediaStore::ensureOwnerDirectory(const QString& path) {
    const QFileInfo existing(path);
    if (existing.isSymLink() || (existing.exists() && !existing.isDir()))
        return false;
    if (!QDir().mkpath(path))
        return false;
    const QFileInfo created(path);
    return created.isDir() && !created.isSymLink() && QFile::setPermissions(path, ownerDirectory);
}

McpMediaStore::McpMediaStore(QString directory, const qint64 capacityBytes)
    : directory_(std::move(directory)), capacityBytes_(capacityBytes) {}

QString McpMediaStore::directoryForJournal(const QString& journalPath) {
    return QFileInfo(journalPath).absoluteDir().filePath(QStringLiteral("media"));
}

QJsonObject McpMediaStore::externalizeItem(const QJsonObject& item, const QString& sessionId,
                                           const QString& worktree) {
    QJsonObject result = item.value(QStringLiteral("result")).toObject();
    const QString itemType = item.value(QStringLiteral("type")).toString();
    if (itemType == QStringLiteral("imageView") || itemType == QStringLiteral("imageGeneration")) {
        QString path =
            item.value(itemType == QStringLiteral("imageView") ? QStringLiteral("path")
                                                               : QStringLiteral("savedPath"))
                .toString();
        if (!path.isEmpty() && !QDir::isAbsolutePath(path) && !worktree.isEmpty())
            path = QDir(worktree).absoluteFilePath(path);
        QJsonObject block = omitted(QStringLiteral("image"), QStringLiteral("source_unavailable"));
        QFile source;
        qint64 size = 0;
        // Only regular local files, bounded before and during the read. The provider already
        // viewed/generated this file; remote readers still need the journal's session grant.
        if (QDir::isAbsolutePath(path) && openRegularFile(path, &source, &size)) {
            if (size > maximumImageBytes) {
                block = omitted(QStringLiteral("image"), QStringLiteral("too_large"));
            } else {
                const QByteArray bytes = source.read(maximumImageBytes + 1);
                const QString mime = bytes.startsWith("\x89PNG\r\n\x1a\n")
                                         ? QStringLiteral("image/png")
                                         : QStringLiteral("image/jpeg");
                const QJsonObject stored = storeImage(bytes, mime, sessionId);
                block = stored.isEmpty()
                            ? omitted(QStringLiteral("image"), QStringLiteral("invalid_image"))
                            : stored;
            }
        }
        result.insert(QStringLiteral("content"), QJsonArray{block});
        QJsonObject externalized = item;
        externalized.insert(QStringLiteral("result"), result);
        return externalized;
    }
    const QJsonArray content = result.value(QStringLiteral("content")).toArray();
    bool changed = false;
    int stored = 0;
    QStringList hashes;
    QList<QPair<qsizetype, QString>> storedBlocks;
    QJsonArray blocks;
    for (const QJsonValue& value : content) {
        const QJsonObject block = value.toObject();
        const QString type = block.value(QStringLiteral("type")).toString();
        // Embedded resources ({type:"resource", resource:{uri, mimeType, blob}}) carry base64
        // too; image blobs are stored like image blocks, every other blob is dropped.
        const QJsonObject resource = block.value(QStringLiteral("resource")).toObject();
        const bool resourceBlob =
            type == QStringLiteral("resource") && resource.value(QStringLiteral("blob")).isString();
        if (type != QStringLiteral("image") && type != QStringLiteral("audio") && !resourceBlob) {
            blocks.append(value);
            continue;
        }
        changed = true;
        const QString mimeType = resourceBlob
                                     ? resource.value(QStringLiteral("mimeType")).toString()
                                     : block.value(QStringLiteral("mimeType")).toString();
        if (type == QStringLiteral("audio") ||
            (resourceBlob && !mimeType.startsWith(QStringLiteral("image/")))) {
            blocks.append(omitted(type, QStringLiteral("unsupported_media")));
            continue;
        }
        if (stored >= maximumImagesPerCall) {
            blocks.append(omitted(type, QStringLiteral("too_many_images")));
            continue;
        }
        const QString data = resourceBlob ? resource.value(QStringLiteral("blob")).toString()
                                          : block.value(QStringLiteral("data")).toString();
        if (data.size() > (maximumImageBytes / 3 + 1) * 4) {
            blocks.append(omitted(type, QStringLiteral("too_large")));
            continue;
        }
        const auto decoded = QByteArray::fromBase64Encoding(
            data.toLatin1(), QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.isEmpty() || decoded.decoded.size() > maximumImageBytes) {
            blocks.append(omitted(type, QStringLiteral("invalid")));
            continue;
        }
        const QByteArray& bytes = decoded.decoded;
        const QString extension = extensionFor(mimeType, bytes);
        if (extension.isEmpty()) {
            blocks.append(omitted(type, QStringLiteral("unsupported_media")));
            continue;
        }
        const QJsonObject reference = write(bytes, extension);
        if (reference.isEmpty()) {
            blocks.append(omitted(type, QStringLiteral("storage_unavailable")));
            continue;
        }
        ++stored;
        hashes.append(reference.value(QStringLiteral("media_ref")).toString().mid(7));
        storedBlocks.append({blocks.size(), type});
        blocks.append(reference);
    }
    if (!changed)
        return item;
    if (!reference(sessionId, hashes)) {
        for (const auto& [index, type] : storedBlocks)
            blocks[index] = omitted(type, QStringLiteral("storage_unavailable"));
    }
    result.insert(QStringLiteral("content"), blocks);
    QJsonObject externalized = item;
    externalized.insert(QStringLiteral("result"), result);
    return externalized;
}

QJsonObject McpMediaStore::storeImage(const QByteArray& bytes, const QString& mimeType,
                                      const QString& sessionId) {
    if (bytes.isEmpty() || bytes.size() > maximumImageBytes)
        return {};
    const QString extension = extensionFor(mimeType, bytes);
    if (extension.isEmpty())
        return {};
    const QJsonObject stored = write(bytes, extension);
    if (!stored.isEmpty() &&
        !reference(sessionId, {stored.value(QStringLiteral("media_ref")).toString().mid(7)}))
        return {};
    return stored;
}

QJsonObject McpMediaStore::write(const QByteArray& bytes, const QString& extension) const {
    const QString hex =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const QString path = QDir(directory_).filePath(hex + QLatin1Char('.') + extension);
    bool ok = ensureOwnerDirectory(directory_);
    const QFileInfo info(path);
    if (info.isSymLink())
        return {};
    if (ok && info.exists()) {
        // A content-addressed name alone does not prove that the stored bytes
        // still match. Preserve unsupported entries for repair, never overwrite.
        if (!info.isFile() || info.size() != bytes.size())
            return {};
        QFile existing(path);
        ok = existing.open(QIODevice::ReadOnly) && existing.read(maximumImageBytes + 1) == bytes &&
             existing.error() == QFileDevice::NoError && existing.setPermissions(ownerFile);
        if (ok) // LRU refresh is best effort; content and privacy are mandatory.
            existing.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    } else if (ok) {
        QSaveFile file(path);
        ok = file.open(QIODevice::WriteOnly) && file.setPermissions(ownerFile) &&
             file.write(bytes) == bytes.size() && file.commit();
    }
    if (!ok)
        return {};
    QJsonObject reference{{QStringLiteral("type"), QStringLiteral("image")},
                          {QStringLiteral("media_ref"), QStringLiteral("sha256:") + hex},
                          {QStringLiteral("mime"), extension == QStringLiteral("png")
                                                       ? QStringLiteral("image/png")
                                                       : QStringLiteral("image/jpeg")},
                          {QStringLiteral("bytes"), static_cast<qint64>(bytes.size())}};
    const QSize size = imageSize(extension, bytes);
    if (size.width() > 0 && size.height() > 0) {
        reference.insert(QStringLiteral("width"), size.width());
        reference.insert(QStringLiteral("height"), size.height());
    }
    return reference;
}

} // namespace ketplus::daemon
