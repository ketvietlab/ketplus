#include "storage/McpMediaStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <utility>

namespace ketplus::daemon {
namespace {

constexpr QFileDevice::Permissions ownerFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

constexpr qint64 maximumReferenceFileBytes = 4 * 1024 * 1024;
constexpr qint64 maximumReferenceCatalogBytes = 16 * 1024 * 1024;
constexpr int maximumReferenceEntries = 4096;
using ReferenceCatalog = QHash<QString, QSet<QString>>;

bool isDigest(const QByteArray& value) {
    return value.size() == 64 && std::all_of(value.cbegin(), value.cend(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

enum class ReferenceDirectoryState { Missing, Ready, Unknown };

ReferenceDirectoryState referenceDirectoryState(const QString& directory) {
    try {
        std::error_code failure;
        const auto status = std::filesystem::symlink_status(
            std::filesystem::u8path(directory.toUtf8().toStdString()), failure);
        if (status.type() == std::filesystem::file_type::not_found &&
            (!failure || failure == std::errc::no_such_file_or_directory))
            return ReferenceDirectoryState::Missing;
        if (failure || !std::filesystem::is_directory(status) ||
            std::filesystem::is_symlink(status))
            return ReferenceDirectoryState::Unknown;
        return ReferenceDirectoryState::Ready;
    } catch (const std::filesystem::filesystem_error&) {
        return ReferenceDirectoryState::Unknown;
    }
}

struct ReferenceFile {
    bool present = false;
    qsizetype bytes = 0;
    QSet<QString> hashes;
};

std::optional<ReferenceFile> readReferenceFile(const QString& path, bool allowMissing) {
    try {
        std::error_code failure;
        const auto status = std::filesystem::symlink_status(
            std::filesystem::u8path(path.toUtf8().toStdString()), failure);
        if (allowMissing && status.type() == std::filesystem::file_type::not_found &&
            (!failure || failure == std::errc::no_such_file_or_directory))
            return ReferenceFile{};
        if (failure || !std::filesystem::is_regular_file(status) ||
            QFileInfo(path).size() > maximumReferenceFileBytes)
            return std::nullopt;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return std::nullopt;
        const auto bytes = file.read(maximumReferenceFileBytes + 1);
        if (file.error() != QFileDevice::NoError || bytes.size() > maximumReferenceFileBytes)
            return std::nullopt;
        ReferenceFile result{true, bytes.size(), {}};
        for (const auto& line : bytes.split('\n')) {
            if (line.isEmpty())
                continue;
            if (!isDigest(line))
                return std::nullopt;
            result.hashes.insert(QString::fromLatin1(line));
        }
        return result;
    } catch (const std::filesystem::filesystem_error&) {
        return std::nullopt;
    }
}

bool writeReferenceFile(const QString& path, const QSet<QString>& hashes) {
    auto sorted = hashes.values();
    std::sort(sorted.begin(), sorted.end());
    const auto bytes = sorted.join(QLatin1Char('\n')).toLatin1() + '\n';
    if (bytes.size() > maximumReferenceFileBytes)
        return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(ownerFile) &&
           file.write(bytes) == bytes.size() && file.commit();
}

std::optional<ReferenceCatalog> readReferenceCatalog(const QString& directory) {
    try {
        const auto path = std::filesystem::u8path(directory.toUtf8().toStdString());
        std::error_code failure;
        const auto state = referenceDirectoryState(directory);
        if (state == ReferenceDirectoryState::Missing)
            return ReferenceCatalog{};
        if (state != ReferenceDirectoryState::Ready)
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
            if (!isDigest(info.completeBaseName().toLatin1()))
                return std::nullopt;
            auto file = readReferenceFile(info.absoluteFilePath(), false);
            if (!file || totalBytes + file->bytes > maximumReferenceCatalogBytes)
                return std::nullopt;
            totalBytes += file->bytes;
            result.insert(info.fileName(), std::move(file->hashes));
        }
        if (failure)
            return std::nullopt;
        return result;
    } catch (const std::filesystem::filesystem_error&) {
        return std::nullopt;
    }
}

} // namespace

QString McpMediaStore::refsPath(const QString& sessionId) const {
    return QDir(directory_)
        .filePath(
            QStringLiteral("refs/") +
            QString::fromLatin1(
                QCryptographicHash::hash(sessionId.toUtf8(), QCryptographicHash::Sha256).toHex()) +
            QStringLiteral(".refs"));
}

bool McpMediaStore::copySessionReferences(const QString& fromSessionId, const QString& toSessionId,
                                          QString* error) {
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    const QString source = refsPath(fromSessionId);
    const QString target = refsPath(toSessionId);
    const auto directory = QFileInfo(source).absolutePath();
    const QFileInfo root(directory_);
    if (root.isSymLink() || (root.exists() && !root.isDir()))
        return fail(QStringLiteral("The media store root is unsupported."));
    const auto state = referenceDirectoryState(directory);
    if (state == ReferenceDirectoryState::Missing)
        return true;
    if (state != ReferenceDirectoryState::Ready)
        return fail(QStringLiteral("The media ownership directory cannot be read."));
    const auto from = readReferenceFile(source, true);
    const auto to = readReferenceFile(target, true);
    if (!from || !to)
        return fail(QStringLiteral("The source or destination media ownership is unsupported."));
    if (to->present) {
        if (to->hashes != from->hashes)
            return fail(
                QStringLiteral("The fork destination already has different media ownership."));
        // A previous journal commit may have failed after preparing these exact
        // references. Retry without replacing or widening destination ownership.
        return QFile::setPermissions(target, ownerFile) ||
               fail(QStringLiteral("The fork media permissions cannot be set."));
    }
    if (from->hashes.isEmpty())
        return true;
    if (!ensureOwnerDirectory(directory_) || !ensureOwnerDirectory(directory) ||
        !writeReferenceFile(target, from->hashes))
        return fail(QStringLiteral("The fork media ownership could not be stored."));
    return true;
}

bool McpMediaStore::reference(const QString& sessionId, const QStringList& hashes) {
    if (hashes.isEmpty())
        return true;
    const QString refs = refsPath(sessionId);
    if (!ensureOwnerDirectory(QFileInfo(refs).absolutePath()))
        return false;
    auto existing = readReferenceFile(refs, true);
    if (!existing)
        return false;
    auto owned = std::move(existing->hashes);
    for (const auto& hash : hashes) {
        if (!isDigest(hash.toLatin1()))
            return false;
        owned.insert(hash);
    }
    if (!writeReferenceFile(refs, owned))
        return false;
    enforceCapacity();
    return true;
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
