#pragma once

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVector>

#include <atomic>
#include <memory>
#include <optional>

namespace ketplus {

struct AtlasScreenDescriptor {
    QString id;
    QString title;
    QString url;
    QString localPath;
    bool remote{false};

    bool operator==(const AtlasScreenDescriptor&) const = default;
};

struct AtlasFlowDescriptor {
    QString id;
    QString title;
    int screenCount{0}; // Distinct screens its nodes show.

    bool operator==(const AtlasFlowDescriptor&) const = default;
};

struct AtlasDescriptor {
    QString path;
    QString workspaceRoot;
    QString directory;
    QString relativePath;
    QString title;
    QVector<AtlasScreenDescriptor> screens;
    int flowCount{0};
    qint64 modified{0};
    qint64 size{0};
    QString description;
    QVector<AtlasFlowDescriptor> flows;

    QString atlasId() const { return path; }
    bool operator==(const AtlasDescriptor&) const = default;
};

class AtlasIndex final : public QObject {
    Q_OBJECT

  public:
    explicit AtlasIndex(QObject* parent = nullptr);
    ~AtlasIndex() override;

    void setRoot(const QString& root);
    void refresh();
    void scheduleRefresh(int delayMilliseconds = 350);

    bool isIndexing() const { return worker_.isRunning(); }
    const QVector<AtlasDescriptor>& manifests() const { return manifests_; }
    QVector<AtlasDescriptor> atlases() const;
    const QString& workspaceRoot() const { return workspaceRoot_; }

    // Limits of the directory walk used when the root is not a Git checkout.
    static constexpr int maximumScannedDirectories = 10'000;
    static constexpr qint64 maximumScanMilliseconds = 2'000;

    // `truncated` reports a directory walk that stopped at its limits.
    static QVector<AtlasDescriptor> scan(const QString& root,
                                         const std::shared_ptr<std::atomic_bool>& cancelled = {},
                                         bool* truncated = nullptr);
    // Inspect one explicit local result without scanning other bundles.
    static AtlasDescriptor readManifest(const QString& path, const QString& root);
    static constexpr qint64 maximumManifestBytes = 4 * 1024 * 1024;
    static QVector<AtlasDescriptor> visibleAtlases(const QVector<AtlasDescriptor>& manifests);

  signals:
    void changed();

  private:
    struct CacheEntry {
        qint64 modified{0};
        qint64 size{0};
        std::optional<AtlasDescriptor> descriptor;
    };

    struct Snapshot {
        QVector<AtlasDescriptor> manifests;
        QHash<QString, CacheEntry> cache;
        QString root;
        qint64 rootModified{0};
        quint64 generation{0};
        bool truncated{false};
    };

    static Snapshot scanSnapshot(const QString& root,
                                 const std::shared_ptr<std::atomic_bool>& cancelled,
                                 const QHash<QString, CacheEntry>& previous, quint64 generation);
    void updateWatcher();
    void verifyWatchedState();

    QString root_;
    QString workspaceRoot_;
    QVector<AtlasDescriptor> manifests_;
    QHash<QString, CacheEntry> cache_;
    QFutureWatcher<Snapshot> worker_;
    QFileSystemWatcher watcher_;
    QTimer debounce_;
    QTimer verification_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    quint64 generation_{0};
    qint64 observedRootModified_{0};
    bool pending_{false};
};

} // namespace ketplus
