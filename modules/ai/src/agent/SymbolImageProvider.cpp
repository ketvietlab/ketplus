#include "agent/SymbolImageProvider.h"
#include "KetIcons.h"
namespace ketplus {
SymbolImageProvider::SymbolImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
QImage SymbolImageProvider::requestImage(const QString& id, QSize* size,
                                         const QSize& requestedSize) {
    const qsizetype separator = id.lastIndexOf(QLatin1Char('/'));
    if (separator <= 0)
        return {};
    const QString name = id.left(separator);
    const QString code = ket::Icons::contains(name) ? name : ket::Icons::codeForLucide(name);
    const qreal box = id.mid(separator + 1).toDouble();
    if (code.isEmpty() || box <= 0 || box > 512)
        return {};
    const qreal dpr = requestedSize.isEmpty() ? 1.0 : requestedSize.width() / box;
    QImage image = ket::Icons::pixmap(code, box, dpr, Qt::white).toImage();
    if (size)
        *size = image.size();
    return image;
}
} // namespace ketplus
