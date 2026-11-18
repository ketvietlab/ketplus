#include "runner/sync/PackCache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <utility>
#include <vector>

namespace ketplus::daemon::runner::sync {

namespace {

constexpr qint64 maximumSideFileBytes = 4096;

QString digestOf(const QString& text, const int length) {
    return QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex().left(length));
}

bool isDigest(const QString& value) {
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    return hex.match(value).hasMatch();
}

// The job stem a cache file belongs to: the part of its name before the first '-' or '.'.
QString stemOfFile(const QString& name) {
    const qsizetype end = name.indexOf(QRegularExpression(QStringLiteral("[-.]")));
    return end < 0 ? name : name.left(end);
}

} // namespace

PackCache::PackCache(QString directory, PackCacheLimits limits)
    : directory_(std::move(directory)), limits_(limits) {}

QString PackCache::stem(const QString& jobId) const { return digestOf(jobId, 32); }

QString PackCache::pathFor(const QString& jobId, const QString& snapshotId,
                           const QString& baseId) const {
    return QDir(directory_)
        .filePath(stem(jobId) + QLatin1Char('-') +
                  digestOf(snapshotId + QLatin1Char(' ') + baseId, 16) + QStringLiteral(".pack"));
}

bool PackCache::record(const QString& jobId, const PackFile& pack, const QString& runnerId) {
    if (pack.path != pathFor(jobId, pack.snapshotId, pack.baseId) || !isDigest(pack.sha256) ||
        pack.size <= 0)
        return false;
    if (const auto previous = find(jobId); previous && previous->path != pack.path)
        QFile::remove(previous->path);
    if (!QDir().mkpath(directory_))
        return false;
    QJsonObject side{{QStringLiteral("version"), 1},
                     {QStringLiteral("jobId"), jobId},
                     {QStringLiteral("snapshotId"), pack.snapshotId},
                     {QStringLiteral("baseId"), pack.baseId},
                     {QStringLiteral("size"), pack.size},
                     {QStringLiteral("sha256"), pack.sha256}};
    if (!runnerId.isEmpty())
        side.insert(QStringLiteral("runner"), digestOf(runnerId, 32));
    QSaveFile file(QDir(directory_).filePath(stem(jobId) + QStringLiteral(".json")));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(side).toJson(QJsonDocument::Compact));
    return file.commit();
}

std::optional<PackFile> PackCache::find(const QString& jobId) const {
    QFile file(QDir(directory_).filePath(stem(jobId) + QStringLiteral(".json")));
    if (file.size() > maximumSideFileBytes || !file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonObject side = QJsonDocument::fromJson(file.readAll()).object();
    if (side.value(QStringLiteral("version")).toInt() != 1 ||
        side.value(QStringLiteral("jobId")).toString() != jobId)
        return std::nullopt;
    PackFile pack;
    pack.snapshotId = side.value(QStringLiteral("snapshotId")).toString();
    pack.baseId = side.value(QStringLiteral("baseId")).toString();
    pack.size = side.value(QStringLiteral("size")).toInteger();
    pack.sha256 = side.value(QStringLiteral("sha256")).toString();
    pack.path = pathFor(jobId, pack.snapshotId, pack.baseId);
    const QFileInfo info(pack.path);
    if (pack.snapshotId.isEmpty() || !isDigest(pack.sha256) || info.isSymLink() || !info.isFile() ||
        info.size() != pack.size)
        return std::nullopt;
    return pack;
}

void PackCache::remove(const QString& jobId) {
    const QDir directory(directory_);
    const QStringList names =
        directory.entryList({stem(jobId) + QStringLiteral("*")}, QDir::Files | QDir::Hidden);
    for (const QString& name : names)
        QFile::remove(directory.filePath(name));
}

void PackCache::setInFlight(const QString& jobId, const bool inFlight) {
    if (inFlight)
        inFlight_.insert(stem(jobId));
    else
        inFlight_.remove(stem(jobId));
}

bool PackCache::inFlight(const QString& jobId) const { return inFlight_.contains(stem(jobId)); }

void PackCache::setLiveJobs(const QStringList& jobIds) {
    for (const QString& jobId : jobIds)
        inFlight_.insert(stem(jobId));
    liveKnown_ = true;
}

void PackCache::removeRunner(const QString& runnerId) {
    const QString runner = digestOf(runnerId, 32);
    const QDir directory(directory_);
    const QStringList sides = directory.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString& name : sides) {
        const QString key = stemOfFile(name);
        if (inFlight_.contains(key))
            continue;
        QFile file(directory.filePath(name));
        if (file.size() > maximumSideFileBytes || !file.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject side = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        if (side.value(QStringLiteral("runner")).toString() != runner)
            continue;
        const QStringList names =
            directory.entryList({key + QStringLiteral("*")}, QDir::Files | QDir::Hidden);
        for (const QString& owned : names)
            QFile::remove(directory.filePath(owned));
    }
}

void PackCache::sweep(const QDateTime& now) {
    struct Group {
        QStringList paths;
        qint64 bytes = 0;
        QDateTime newest;
        bool recorded = false; // has a side file
    };
    QMap<QString, Group> groups;
    const QDir directory(directory_);
    const auto entries =
        directory.entryInfoList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
    for (const QFileInfo& info : entries) {
        Group& group = groups[stemOfFile(info.fileName())];
        group.paths.append(info.filePath());
        group.bytes += info.size();
        group.recorded = group.recorded || info.suffix() == QStringLiteral("json");
        if (!group.newest.isValid() || info.lastModified() > group.newest)
            group.newest = info.lastModified();
    }
    const QDateTime expired = now.addSecs(-limits_.maxAgeSeconds);
    qint64 total = 0;
    std::vector<std::pair<QDateTime, QString>> evictable;
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        const bool held = inFlight_.contains(it.key()) || (!liveKnown_ && it->recorded);
        if (!held && it->newest < expired) {
            for (const QString& path : std::as_const(it->paths))
                QFile::remove(path);
            continue;
        }
        total += it->bytes;
        if (!held)
            evictable.emplace_back(it->newest, it.key());
    }
    std::sort(evictable.begin(), evictable.end());
    for (const auto& [when, key] : evictable) {
        if (total <= limits_.maxBytes)
            break;
        const Group& group = groups[key];
        for (const QString& path : group.paths)
            QFile::remove(path);
        total -= group.bytes;
    }
}

qint64 PackCache::totalBytes() const {
    qint64 total = 0;
    const auto entries = QDir(directory_).entryInfoList(QDir::Files | QDir::Hidden | QDir::System);
    for (const QFileInfo& info : entries)
        total += info.size();
    return total;
}

} // namespace ketplus::daemon::runner::sync
