#include "atlas/AtlasThumbnailCache.h"
#include "KetTokens.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPainter>
#include <QtConcurrent/QtConcurrentRun>

namespace ketplus {

AtlasThumbnailCache::AtlasThumbnailCache(QObject* parent) : QObject(parent) {
    pool_.setMaxThreadCount(2);
    pool_.setExpiryTimeout(30'000);
}

AtlasThumbnailCache::~AtlasThumbnailCache() {
    pool_.clear();
    pool_.waitForDone();
}

QString AtlasThumbnailCache::cacheKey(const AtlasDescriptor& atlas) {
    const QString screen = atlas.screens.isEmpty() ? QString{} : atlas.screens.first().localPath;
    const qint64 screenModified =
        screen.isEmpty() ? 0 : QFileInfo(screen).lastModified().toMSecsSinceEpoch();
    return QStringLiteral("%1:%2:%3:%4")
        .arg(atlas.path)
        .arg(atlas.modified)
        .arg(screen)
        .arg(screenModified);
}

QImage AtlasThumbnailCache::request(const AtlasDescriptor& atlas) {
    const QString key = cacheKey(atlas);
    if (auto* cached = cache_.object(key))
        return *cached;
    if (pending_.contains(key) || pending_.size() >= maximumPending)
        return {};

    pending_.insert(key);
    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, key] {
        const QImage image = watcher->result();
        pending_.remove(key);
        watcher->deleteLater();
        if (image.isNull())
            return;
        insert(key, image);
        emit thumbnailReady(key, image);
    });
    watcher->setFuture(QtConcurrent::run(&pool_, [atlas] { return render(atlas); }));
    return {};
}

void AtlasThumbnailCache::insert(const QString& key, const QImage& image) {
    const int bytes = qMax(1, int(image.sizeInBytes()));
    if (bytes > maximumBytes)
        return;
    // QCache evicts internally. Recompute the exact retained bytes so the diagnostic
    // counter cannot drift after replacement or LRU eviction.
    cache_.insert(key, new QImage(image), bytes);
    cachedBytes_ = 0;
    const auto keys = cache_.keys();
    for (const auto& retained : keys)
        if (const auto* value = cache_.object(retained))
            cachedBytes_ += int(value->sizeInBytes());
}

QImage AtlasThumbnailCache::render(const AtlasDescriptor& atlas) {
    constexpr int width = 440;
    constexpr int height = 240;
    if (!atlas.screens.isEmpty()) {
        const QImage source(atlas.screens.first().localPath);
        if (!source.isNull())
            return source
                .scaled(width, height, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                .copy(QRect(QPoint(), QSize(width, height)));
    }

    const QByteArray digest =
        QCryptographicHash::hash(atlas.path.toUtf8(), QCryptographicHash::Sha256);
    const int hue = uchar(digest.at(0)) * 360 / 255;
    const QColor accent = QColor::fromHsl(hue, 120, 128);
    // A cached placeholder illustration uses the stable light token palette.
    const auto color = [](QStringView token) {
        return ket::Tokens::color(token, ket::Theme::Light);
    };
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(color(u"page-bg"));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(QRect(0, 0, width, 26), color(u"surface-raised"));
    for (int x = 12; x < 42; x += 10) {
        painter.setBrush(color(u"text-muted"));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(x, 13), 2.5, 2.5);
    }
    painter.fillRect(QRect(0, 26, 54, height - 26), color(u"sidebar-bg"));
    painter.fillRect(QRect(12, 46, 30, 5), accent);
    painter.setBrush(color(u"panel-bg"));
    painter.setPen(QPen(color(u"panel-border"), 1));
    painter.drawRoundedRect(QRectF(78, 50, 144, 76), 7, 7);
    painter.drawRoundedRect(QRectF(244, 50, 164, 76), 7, 7);
    painter.drawRoundedRect(QRectF(78, 145, 330, 62), 7, 7);
    painter.fillRect(QRect(92, 65, 74, 8), accent.lighter(135));
    painter.fillRect(QRect(92, 82, 102, 5), color(u"border-strong"));
    painter.fillRect(QRect(258, 65, 112, 8), accent);
    painter.fillRect(QRect(258, 82, 126, 5), color(u"border-strong"));
    painter.fillRect(QRect(92, 160, 190, 7), color(u"text-muted"));
    painter.fillRect(QRect(92, 178, 270, 5), color(u"panel-border"));
    return image;
}

} // namespace ketplus
