#pragma once

#include <QQuickImageProvider>

namespace ketplus {
// Resolves legacy source names through Két Symbols, using the kit optical masters.
// IDs are "<name>/<logical width>"; TintedIcon builds these URLs.
class SymbolImageProvider final : public QQuickImageProvider {
  public:
    SymbolImageProvider();
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};
} // namespace ketplus
