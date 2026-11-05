#pragma once

#include "workspace/AtlasIndex.h"

#include <QCache>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QThreadPool>

namespace ketplus {

// Produces lightweight library artwork without starting KetAtlas or a WebView.
// Work is bounded to two threads and the in-memory LRU has a hard byte budget.
class AtlasThumbnailCache final : public QObject {
    Q_OBJECT

  public:
    explicit AtlasThumbnailCache(QObject* parent = nullptr);
    ~AtlasThumbnailCache() override;

    QImage request(const AtlasDescriptor& atlas);
    int cachedBytes() const { return cachedBytes_; }
    int pendingCount() const { return pending_.size(); }
    static QString cacheKey(const AtlasDescriptor& atlas);

  signals:
    void thumbnailReady(const QString& key, const QImage& image);

  private:
    static QImage render(const AtlasDescriptor& atlas);
    void insert(const QString& key, const QImage& image);

    static constexpr int maximumBytes = 64 * 1024 * 1024;
    static constexpr int maximumPending = 32;
    QCache<QString, QImage> cache_{maximumBytes};
    QSet<QString> pending_;
    QThreadPool pool_;
    int cachedBytes_{0};
};

} // namespace ketplus
