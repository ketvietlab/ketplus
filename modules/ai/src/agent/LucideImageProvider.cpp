#include "agent/LucideImageProvider.h"
#include "ui/LucideIcon.h"

#include <QFile>
#include <QPainter>
#include <QSvgRenderer>

namespace ketplus {

LucideImageProvider::LucideImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage LucideImageProvider::requestImage(const QString& id, QSize* size,
                                         const QSize& requestedSize) {
    const qsizetype separator = id.lastIndexOf(QLatin1Char('/'));
    if (separator <= 0)
        return {};
    QFile file(QStringLiteral(":/agent/icons/%1.svg").arg(id.left(separator)));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QSvgRenderer renderer(
        lucide_icon::withStrokeWidth(file.readAll(), id.mid(separator + 1).toDouble()));
    const QSize target = requestedSize.isEmpty() ? renderer.defaultSize() : requestedSize;
    QImage image(target, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    renderer.render(&painter);
    if (size)
        *size = target;
    return image;
}

} // namespace ketplus
