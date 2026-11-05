#include "workspace/AtlasIndex.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace ketplus {
namespace {
constexpr qint64 maximumManifestSize = 4 * 1024 * 1024;
const QString atlasDirectorySuffix = QStringLiteral(".ketatlas");
const QString atlasManifestName = QStringLiteral("atlas.json");

const QSet<QString> excludedDirectories{
    QStringLiteral(".git"),  QStringLiteral("node_modules"), QStringLiteral("build"),
    QStringLiteral("dist"),  QStringLiteral(".next"),        QStringLiteral(".cache"),
    QStringLiteral(".venv"), QStringLiteral("__pycache__"),  QStringLiteral("vendor")};

bool isExcluded(const QString& relativePath) {
    for (const auto& part : relativePath.split('/'))
        if (excludedDirectories.contains(part))
            return true;
    return false;
}

QString localScreenPath(const QString& value, const QString& manifestPath, const QString& root) {
    const QUrl url(value);
    if (!url.scheme().isEmpty() || value.startsWith(QStringLiteral("//")) || value.startsWith('#'))
        return {};

    const QString path = url.path(QUrl::FullyDecoded);
    if (path.isEmpty())
        return {};
    const QString absolute = path.startsWith('/') ? QDir(root).filePath(path.mid(1))
                                                  : QFileInfo(manifestPath).dir().filePath(path);
    return QDir::cleanPath(QFileInfo(absolute).absoluteFilePath());
}

QFileInfoList gitJsonFiles(const QString& root, const std::shared_ptr<std::atomic_bool>& cancelled,
                           bool& succeeded) {
    succeeded = false;
    QProcess git;
    git.start(QStringLiteral("git"),
              {QStringLiteral("-C"), root, QStringLiteral("ls-files"), QStringLiteral("--cached"),
               QStringLiteral("--others"), QStringLiteral("--exclude-standard"),
               QStringLiteral("-z"), QStringLiteral("--"),
               QStringLiteral(":(glob,icase)**/*.ketatlas/atlas.json")});
    while (!git.waitForFinished(100)) {
        if (git.state() == QProcess::NotRunning)
            break;
        if (cancelled && cancelled->load()) {
            git.kill();
            git.waitForFinished();
            return {};
        }
    }
    if (git.exitStatus() != QProcess::NormalExit || git.exitCode() != 0)
        return {};
    succeeded = true;

    QFileInfoList files;
    for (const auto& path : git.readAllStandardOutput().split('\0'))
        if (!path.isEmpty())
            files.append(QFileInfo(QDir(root).filePath(QString::fromUtf8(path))));
    return files;
}

// Without Git there is no ignore list, and the root may be a home or data folder, so the walk is
// bounded and skips hidden folders and ~/Library (listing it can raise macOS privacy prompts).
QFileInfoList directoryJsonFiles(const QString& root,
                                 const std::shared_ptr<std::atomic_bool>& cancelled,
                                 bool& truncated) {
    truncated = false;
    const QString library = QDir(QDir::homePath()).filePath(QStringLiteral("Library"));
    QElapsedTimer elapsed;
    elapsed.start();
    QFileInfoList files;
    QStringList directories{root};
    int visited = 0;
    while (!directories.isEmpty()) {
        if (cancelled && cancelled->load())
            return {};
        if (visited >= AtlasIndex::maximumScannedDirectories ||
            elapsed.elapsed() >= AtlasIndex::maximumScanMilliseconds) {
            truncated = true;
            break;
        }
        ++visited;
        const QString directory = directories.takeLast();
        if (QFileInfo(directory).fileName().endsWith(atlasDirectorySuffix, Qt::CaseInsensitive)) {
            const QFileInfo manifest(QDir(directory).filePath(atlasManifestName));
            if (manifest.isFile() && !manifest.isSymLink())
                files.append(manifest);
            continue;
        }
        for (const auto& info : QDir(directory).entryInfoList(
                 QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks,
                 QDir::Name)) {
            if (excludedDirectories.contains(info.fileName()) || !info.isDir() ||
                (info.fileName().startsWith(u'.') &&
                 !info.fileName().endsWith(atlasDirectorySuffix, Qt::CaseInsensitive)) ||
                info.absoluteFilePath() == library)
                continue;
            directories.append(info.absoluteFilePath());
        }
    }
    return files;
}

AtlasDescriptor parseManifest(const QFileInfo& info, const QString& root) {
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.value(QStringLiteral("version")).toInt() != 1 ||
        !object.value(QStringLiteral("title")).isString() ||
        !object.value(QStringLiteral("screens")).isArray() ||
        !object.value(QStringLiteral("flows")).isArray())
        return {};

    AtlasDescriptor descriptor;
    descriptor.path = info.absoluteFilePath();
    descriptor.workspaceRoot = root;
    descriptor.directory = info.absolutePath();
    descriptor.relativePath = QDir(root).relativeFilePath(descriptor.path);
    descriptor.title = object.value(QStringLiteral("title")).toString();
    descriptor.description = object.value(QStringLiteral("description")).toString();
    descriptor.flowCount = object.value(QStringLiteral("flows")).toArray().size();
    descriptor.modified = info.lastModified().toMSecsSinceEpoch();
    descriptor.size = info.size();
    for (const auto& value : object.value(QStringLiteral("screens")).toArray()) {
        const auto screen = value.toObject();
        const QString id = screen.value(QStringLiteral("id")).toString();
        const QString title = screen.value(QStringLiteral("title")).toString();
        const QString url = screen.value(QStringLiteral("url")).toString();
        if (id.isEmpty() || title.isEmpty() || url.isEmpty())
            continue;
        const QUrl parsedUrl(url);
        const bool remote =
            url.startsWith(QStringLiteral("//")) ||
            parsedUrl.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0 ||
            parsedUrl.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0;
        const QString localPath = localScreenPath(url, descriptor.path, descriptor.directory);
        descriptor.screens.append({id, title, url, localPath, remote});
    }
    for (const auto& value : object.value(QStringLiteral("flows")).toArray()) {
        const auto flow = value.toObject();
        const QString id = flow.value(QStringLiteral("id")).toString();
        const QString title = flow.value(QStringLiteral("title")).toString();
        if (!id.isEmpty())
            descriptor.flows.append({id, title.isEmpty() ? id : title});
    }
    return descriptor;
}
} // namespace

QVector<AtlasDescriptor> AtlasIndex::visibleAtlases(const QVector<AtlasDescriptor>& manifests) {
    QVector<AtlasDescriptor> result;
    for (const auto& manifest : manifests) {
        const auto parts = manifest.relativePath.toLower().split('/');
        const auto name = parts.last();
        if (parts.contains(QStringLiteral("tests")) || parts.contains(QStringLiteral("test")) ||
            parts.contains(QStringLiteral("fixtures")) ||
            parts.contains(QStringLiteral("__fixtures__")) ||
            name.startsWith(QStringLiteral("baseline-")))
            continue;
        result.append(manifest);
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.relativePath < b.relativePath; });
    return result;
}

AtlasIndex::AtlasIndex(QObject* parent) : QObject(parent) {
    debounce_.setSingleShot(true);
    verification_.setInterval(250);
    connect(&debounce_, &QTimer::timeout, this, &AtlasIndex::refresh);
    connect(&verification_, &QTimer::timeout, this, &AtlasIndex::verifyWatchedState);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { scheduleRefresh(); });
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] { scheduleRefresh(); });
    connect(&worker_, &QFutureWatcher<Snapshot>::finished, this, [this] {
        const auto snapshot = worker_.result();
        bool contentsChanged = false;
        if (snapshot.generation == generation_ && (!cancelled_ || !cancelled_->load())) {
            contentsChanged = manifests_ != snapshot.manifests;
            manifests_ = snapshot.manifests;
            cache_ = snapshot.cache;
            workspaceRoot_ = snapshot.root;
            observedRootModified_ = snapshot.rootModified;
            updateWatcher();
        }
        if (contentsChanged)
            emit changed();
        if (pending_) {
            pending_ = false;
            scheduleRefresh(0);
        }
    });
}

AtlasIndex::~AtlasIndex() {
    if (cancelled_)
        cancelled_->store(true);
    worker_.waitForFinished();
}

void AtlasIndex::setRoot(const QString& root) {
    const QFileInfo info(root);
    const QString normalized = info.isDir() ? info.canonicalFilePath() : QString{};
    if (root_ == normalized)
        return;
    ++generation_;
    if (cancelled_)
        cancelled_->store(true);
    root_ = normalized;
    workspaceRoot_ = normalized;
    manifests_.clear();
    cache_.clear();
    observedRootModified_ =
        normalized.isEmpty() ? 0 : QFileInfo(normalized).lastModified().toMSecsSinceEpoch();
    pending_ = worker_.isRunning();
    updateWatcher();
    if (normalized.isEmpty())
        verification_.stop();
    else
        verification_.start();
    emit changed();
    scheduleRefresh(0);
}

void AtlasIndex::scheduleRefresh(const int delayMilliseconds) {
    if (root_.isEmpty())
        return;
    debounce_.start(qMax(0, delayMilliseconds));
}

void AtlasIndex::refresh() {
    debounce_.stop();
    if (root_.isEmpty())
        return;
    if (worker_.isRunning()) {
        pending_ = true;
        return;
    }
    cancelled_ = std::make_shared<std::atomic_bool>(false);
    const auto root = root_;
    const auto previous = cache_;
    const auto cancelled = cancelled_;
    const auto generation = generation_;
    worker_.setFuture(QtConcurrent::run([root, previous, cancelled, generation] {
        return scanSnapshot(root, cancelled, previous, generation);
    }));
}

QVector<AtlasDescriptor> AtlasIndex::atlases() const { return visibleAtlases(manifests_); }

QVector<AtlasDescriptor> AtlasIndex::scan(const QString& root,
                                          const std::shared_ptr<std::atomic_bool>& cancelled,
                                          bool* truncated) {
    Snapshot snapshot = scanSnapshot(root, cancelled, {}, 0);
    if (truncated)
        *truncated = snapshot.truncated;
    return snapshot.manifests;
}

AtlasIndex::Snapshot AtlasIndex::scanSnapshot(const QString& suppliedRoot,
                                              const std::shared_ptr<std::atomic_bool>& cancelled,
                                              const QHash<QString, CacheEntry>& previous,
                                              const quint64 generation) {
    Snapshot snapshot;
    snapshot.generation = generation;
    const QString root = QFileInfo(suppliedRoot).canonicalFilePath();
    if (root.isEmpty())
        return snapshot;
    snapshot.root = root;
    snapshot.rootModified = QFileInfo(root).lastModified().toMSecsSinceEpoch();

    bool gitSucceeded = false;
    QFileInfoList candidates = gitJsonFiles(root, cancelled, gitSucceeded);
    if (!gitSucceeded)
        candidates = directoryJsonFiles(root, cancelled, snapshot.truncated);

    QSet<QString> visited;
    for (const auto& info : candidates) {
        if (cancelled && cancelled->load())
            return snapshot;
        const QString path = info.absoluteFilePath();
        const QString relativePath = QDir(root).relativeFilePath(path);
        if (visited.contains(path) || isExcluded(relativePath) || info.isSymLink() ||
            !info.isFile() || info.size() > maximumManifestSize)
            continue;
        visited.insert(path);

        const qint64 modified = info.lastModified().toMSecsSinceEpoch();
        const auto old = previous.constFind(path);
        if (old != previous.cend() && old->modified == modified && old->size == info.size()) {
            snapshot.cache.insert(path, *old);
            if (old->descriptor)
                snapshot.manifests.append(*old->descriptor);
            continue;
        }

        auto descriptor = parseManifest(info, root);
        CacheEntry entry{modified, info.size(), std::nullopt};
        if (!descriptor.path.isEmpty()) {
            entry.descriptor = std::move(descriptor);
            snapshot.manifests.append(*entry.descriptor);
        }
        snapshot.cache.insert(path, std::move(entry));
    }
    return snapshot;
}

void AtlasIndex::updateWatcher() {
    QSet<QString> desired;
    if (!root_.isEmpty()) {
        desired.insert(root_);
        for (auto entry = cache_.cbegin(); entry != cache_.cend(); ++entry) {
            desired.insert(QFileInfo(entry.key()).absolutePath());
            if (QFileInfo::exists(entry.key()))
                desired.insert(entry.key());
        }
    }
    const auto directories = watcher_.directories();
    const auto files = watcher_.files();
    QSet<QString> current(directories.cbegin(), directories.cend());
    current.unite(QSet<QString>(files.cbegin(), files.cend()));
    // Keep existing watches alive: replacing them can discard pending filesystem
    // events for changes that happened while the background scan was finishing.
    const auto removed = current - desired;
    const auto added = desired - current;
    if (!removed.isEmpty())
        watcher_.removePaths(removed.values());
    if (!added.isEmpty())
        watcher_.addPaths(added.values());
}

void AtlasIndex::verifyWatchedState() {
    if (root_.isEmpty() || worker_.isRunning())
        return;

    const qint64 rootModified = QFileInfo(root_).lastModified().toMSecsSinceEpoch();
    if (rootModified != observedRootModified_) {
        scheduleRefresh(0);
        return;
    }
    for (auto entry = cache_.cbegin(); entry != cache_.cend(); ++entry) {
        const QFileInfo info(entry.key());
        if (!info.exists() || !info.isFile() || info.size() != entry->size ||
            info.lastModified().toMSecsSinceEpoch() != entry->modified) {
            scheduleRefresh(0);
            return;
        }
    }
}

} // namespace ketplus
