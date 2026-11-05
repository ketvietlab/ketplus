#pragma once

#include <QSize>

class QObject;
class QWidget;
class QLayout;
class QBoxLayout;
class QAbstractButton;

namespace ketplus::chrome_geometry {
// Opt-in product geometry, always expressed in Medium logical pixels. These
// bindings never inspect or change the geometry of unregistered CM controls.
void fixedHeight(QWidget* widget, int height);
void fixedWidth(QWidget* widget, int width);
void fixedSize(QWidget* widget, const QSize& size);
inline void fixedSize(QWidget* widget, int width, int height) {
    fixedSize(widget, QSize(width, height));
}
void minimumHeight(QWidget* widget, int height);
void minimumWidth(QWidget* widget, int width);
void maximumWidth(QWidget* widget, int width);
void iconSize(QObject* button, const QSize& size);
void margins(QObject* layout, int left, int top, int right, int bottom);
void spacing(QObject* layout, int spacing);
void horizontalSpacing(QLayout* layout, int spacing);
void verticalSpacing(QLayout* layout, int spacing);
void addSpacing(QBoxLayout* layout, int spacing);
// New bindings inherit the closest product surface's zoom, including rows
// created after the user changes zoom. Existing bindings never compound.
void apply(QWidget* surface, int basePixels);
int basePixels(const QObject* object);
} // namespace ketplus::chrome_geometry
