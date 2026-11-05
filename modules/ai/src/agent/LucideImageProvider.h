#pragma once

#include <QQuickImageProvider>

namespace ketplus {
// Serves the Lucide SVGs under qrc:/agent/icons with the stroke fitted to the size they are shown
// at, so small icons keep a readable line. Ids are "<icon>/<logical width>", e.g. "search/14";
// TintedIcon.qml builds them.
class LucideImageProvider final : public QQuickImageProvider {
  public:
    LucideImageProvider();
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};
} // namespace ketplus
