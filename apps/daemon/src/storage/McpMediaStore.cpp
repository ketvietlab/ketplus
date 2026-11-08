#include "storage/McpMediaStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QSaveFile>
#include <QSet>
#include <QSize>
#include <QtEndian>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <utility>

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

QJsonObject omitted(const QString& type, const QString& reason) {
    return {{QStringLiteral("type"), type},
            {QStringLiteral("omitted"), true},
            {QStringLiteral("reason"), reason}};
}

bool ensureOwnerDirectory(const QString& path) {
    const QFileInfo existing(path);
    if (existing.isSymLink() || (existing.exists() && !existing.isDir()))
        return false;
    if (!QDir().mkpath(path))
        return false;
    const QFileInfo created(path);
    return created.isDir() && !created.isSymLink() && QFile::setPermissions(path, ownerDirectory);
}

constexpr qint64 maximumReferenceFileBytes = 4 * 1024 * 1024;
constexpr qint64 maximumReferenceCatalogBytes = 16 * 1024 * 1024;
constexpr int maximumReferenceEntries = 4096;
using ReferenceCatalog = QHash<QString, QSet<QString>>;

bool isDigest(const QByteArray& value) {
    return value.size() == 64 && std::all_of(value.cbegin(), value.cend(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

std::optional<ReferenceCatalog> readReferenceCatalog(const QString& directory) {
    try {
        const auto path = std::filesystem::u8path(directory.toUtf8().toStdString());
        std::error_code failure;
        const auto root = std::filesystem::symlink_status(path, failure);
        if (root.type() == std::filesystem::file_type::not_found &&
            (!failure || failure == std::errc::no_such_file_or_directory))
            return ReferenceCatalog{};
        if (failure || !std::filesystem::is_directory(root) || std::filesystem::is_symlink(root))
            return std::nullopt;
        ReferenceCatalog result;
        qint64 totalBytes = 0;
        int entries = 0;
        // Count every directory entry, including unknown names, so filtering cannot
        // hide an unbounded traversal. A partial catalog never authorizes deletion.
        std::filesystem::directory_iterator files(path, failure), end;
        for (; !failure && files != end; files.increment(failure)) {
            if (++entries > maximumReferenceEntries)
                return std::nullopt;
            const auto utf8 = files->path().u8string();
            const QFileInfo info(QString::fromUtf8(reinterpret_cast<const char*>(utf8.data()),
                                                   qsizetype(utf8.size())));
            if (!info.fileName().endsWith(QStringLiteral(".refs")))
                continue;
            if (info.isSymLink() || !info.isFile() ||
                !isDigest(info.completeBaseName().toLatin1()) ||
                info.size() > maximumReferenceFileBytes)
                return std::nullopt;
            QFile file(info.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly))
                return std::nullopt;
            const auto bytes = file.read(maximumReferenceFileBytes + 1);
            if (file.error() != QFileDevice::NoError || bytes.size() > maximumReferenceFileBytes ||
                totalBytes + bytes.size() > maximumReferenceCatalogBytes)
                return std::nullopt;
            totalBytes += bytes.size();
            QSet<QString> hashes;
            for (const auto& line : bytes.split('\n')) {
                if (line.isEmpty())
                    continue;
                if (!isDigest(line))
                    return std::nullopt;
                hashes.insert(QString::fromLatin1(line));
            }
            result.insert(info.fileName(), hashes);
        }
        if (failure)
            return std::nullopt;
        return result;
    } catch (const std::filesystem::filesystem_error&) {
        return std::nullopt;
    }
}

} // namespace

McpMediaStore::McpMediaStore(QString directory, const qint64 capacityBytes)
    : directory_(std::move(directory)), capacityBytes_(capacityBytes) {}

QString McpMediaStore::directoryForJournal(const QString& journalPath) {
    return QFileInfo(journalPath).absoluteDir().filePath(QStringLiteral("media"));
}

QString McpMediaStore::refsPath(const QString& sessionId) const {
    return QDir(directory_)
        .filePath(
            QStringLiteral("refs/") +
            QString::fromLatin1(
                QCryptographicHash::hash(sessionId.toUtf8(), QCryptographicHash::Sha256).toHex()) +
            QStringLiteral(".refs"));
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
        const QFileInfo info(path);
        QFile source(path);
        // Only regular local files, bounded before and during the read. The provider already
        // viewed/generated this file; remote readers still need the journal's session grant.
        if (QDir::isAbsolutePath(path) && info.isFile()) {
            if (info.size() > maximumImageBytes) {
                block = omitted(QStringLiteral("image"), QStringLiteral("too_large"));
            } else if (source.open(QIODevice::ReadOnly)) {
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
        blocks.append(reference);
    }
    if (!changed)
        return item;
    result.insert(QStringLiteral("content"), blocks);
    QJsonObject externalized = item;
    externalized.insert(QStringLiteral("result"), result);
    reference(sessionId, hashes);
    return externalized;
}

void McpMediaStore::copySessionReferences(const QString& fromSessionId,
                                          const QString& toSessionId) {
    const QString source = refsPath(fromSessionId);
    const QString target = refsPath(toSessionId);
    if (!QFileInfo::exists(source) || !ensureOwnerDirectory(QFileInfo(target).absolutePath()))
        return;
    QFile::remove(target);
    if (QFile::copy(source, target))
        QFile::setPermissions(target, ownerFile);
}

QJsonObject McpMediaStore::storeImage(const QByteArray& bytes, const QString& mimeType,
                                      const QString& sessionId) {
    if (bytes.isEmpty() || bytes.size() > maximumImageBytes)
        return {};
    const QString extension = extensionFor(mimeType, bytes);
    if (extension.isEmpty())
        return {};
    const QJsonObject stored = write(bytes, extension);
    if (!stored.isEmpty())
        reference(sessionId, {stored.value(QStringLiteral("media_ref")).toString().mid(7)});
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

void McpMediaStore::reference(const QString& sessionId, const QStringList& hashes) {
    if (hashes.isEmpty())
        return;
    const QString refs = refsPath(sessionId);
    if (ensureOwnerDirectory(QFileInfo(refs).absolutePath())) {
        QFile file(refs);
        if (file.open(QIODevice::Append)) {
            file.setPermissions(ownerFile);
            file.write(hashes.join(QLatin1Char('\n')).toLatin1() + '\n');
        }
    }
    enforceCapacity();
}

void McpMediaStore::releaseSession(const QString& sessionId) {
    const QString refs = refsPath(sessionId);
    const auto catalog = readReferenceCatalog(QFileInfo(refs).absolutePath());
    if (!catalog)
        return;
    const auto name = QFileInfo(refs).fileName();
    const auto released = catalog->value(name);
    QSet<QString> retained;
    for (auto entry = catalog->cbegin(); entry != catalog->cend(); ++entry)
        if (entry.key() != name)
            retained.unite(entry.value());
    if (!QFile::remove(refs))
        return;
    for (const QString& hash : released) {
        if (retained.contains(hash))
            continue;
        QFile::remove(QDir(directory_).filePath(hash + QStringLiteral(".png")));
        QFile::remove(QDir(directory_).filePath(hash + QStringLiteral(".jpg")));
    }
}

void McpMediaStore::retainSessions(const QSet<QString>& sessionIds) {
    const QDir refsDirectory(QDir(directory_).filePath(QStringLiteral("refs")));
    const auto catalog = readReferenceCatalog(refsDirectory.path());
    if (!catalog)
        return;
    QSet<QString> live;
    for (const QString& sessionId : sessionIds)
        live.insert(QFileInfo(refsPath(sessionId)).fileName());
    QSet<QString> released;
    QSet<QString> retained;
    for (auto entry = catalog->cbegin(); entry != catalog->cend(); ++entry) {
        if (live.contains(entry.key())) {
            retained.unite(entry.value());
        } else {
            if (!QFile::remove(refsDirectory.filePath(entry.key())))
                return;
            released.unite(entry.value());
        }
    }
    for (const QString& hash : std::as_const(released)) {
        if (retained.contains(hash))
            continue;
        QFile::remove(QDir(directory_).filePath(hash + QStringLiteral(".png")));
        QFile::remove(QDir(directory_).filePath(hash + QStringLiteral(".jpg")));
    }
}

void McpMediaStore::enforceCapacity() {
    const QFileInfoList media =
        QDir(directory_)
            .entryInfoList({QStringLiteral("*.png"), QStringLiteral("*.jpg")}, QDir::Files,
                           QDir::Time | QDir::Reversed);
    qint64 total = 0;
    for (const QFileInfo& file : media)
        total += file.size();
    if (total <= capacityBytes_)
        return;
    const auto catalog = readReferenceCatalog(QDir(directory_).filePath(QStringLiteral("refs")));
    if (!catalog)
        return;
    QSet<QString> referenced;
    for (const auto& hashes : *catalog)
        referenced.unite(hashes);
    // Unreferenced media goes first (oldest first); media a live session still shows is evicted
    // only when that alone cannot bring the store under its cap.
    for (const bool evictReferenced : {false, true}) {
        for (const QFileInfo& file : media) {
            if (total <= capacityBytes_)
                return;
            if (referenced.contains(file.baseName()) != evictReferenced ||
                !QFileInfo::exists(file.absoluteFilePath()))
                continue;
            if (evictReferenced)
                qWarning("MCP media store over capacity; evicting media still referenced by a "
                         "session: %s",
                         qUtf8Printable(file.fileName()));
            if (QFile::remove(file.absoluteFilePath()))
                total -= file.size();
        }
    }
}

} // namespace ketplus::daemon
