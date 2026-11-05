#include "ui/ChromeGeometry.h"
#include "ui/InterfaceZoom.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QBoxLayout>
#include <QGridLayout>
#include <QListView>
#include <QSet>
#include <QVariantMap>
#include <QWidget>

namespace ketplus::chrome_geometry {
namespace {
constexpr auto metricsKey = "_ketChromeMetrics";
constexpr auto zoomKey = "_ketChromeZoom";
void applyObject(QObject* object, int base) {
    const auto values = object->property(metricsKey).toMap();
    const auto px = [base](int value) { return InterfaceZoomMetrics::geometry(value, base); };
    const auto size = [&px](QSize value) { return QSize(px(value.width()), px(value.height())); };
    if (auto* widget = qobject_cast<QWidget*>(object)) {
        widget->ensurePolished();
        if (values.contains("fixedHeight"))
            widget->setFixedHeight(px(values.value("fixedHeight").toInt()));
        if (values.contains("fixedWidth"))
            widget->setFixedWidth(px(values.value("fixedWidth").toInt()));
        if (values.contains("fixedSize"))
            widget->setFixedSize(size(values.value("fixedSize").toSize()));
        if (values.contains("minimumHeight"))
            widget->setMinimumHeight(px(values.value("minimumHeight").toInt()));
        if (values.contains("minimumWidth"))
            widget->setMinimumWidth(px(values.value("minimumWidth").toInt()));
        if (values.contains("maximumWidth"))
            widget->setMaximumWidth(px(values.value("maximumWidth").toInt()));
    }
    if (auto* button = qobject_cast<QAbstractButton*>(object);
        button && values.contains("iconSize"))
        button->setIconSize(size(values.value("iconSize").toSize()));
    if (auto* view = qobject_cast<QAbstractItemView*>(object); view && values.contains("iconSize"))
        view->setIconSize(size(values.value("iconSize").toSize()));
    if (auto* list = qobject_cast<QListView*>(object); list && values.contains("spacing"))
        list->setSpacing(px(values.value("spacing").toInt()));
    if (auto* widget = qobject_cast<QWidget*>(object); widget && values.contains("margins")) {
        const auto m = values.value("margins").toList();
        widget->setContentsMargins(px(m[0].toInt()), px(m[1].toInt()), px(m[2].toInt()),
                                   px(m[3].toInt()));
    }
    if (auto* grid = qobject_cast<QGridLayout*>(object)) {
        if (values.contains("horizontalSpacing"))
            grid->setHorizontalSpacing(px(values.value("horizontalSpacing").toInt()));
        if (values.contains("verticalSpacing"))
            grid->setVerticalSpacing(px(values.value("verticalSpacing").toInt()));
    }
    if (auto* layout = qobject_cast<QLayout*>(object)) {
        if (values.contains("margins")) {
            const auto m = values.value("margins").toList();
            layout->setContentsMargins(px(m[0].toInt()), px(m[1].toInt()), px(m[2].toInt()),
                                       px(m[3].toInt()));
        }
        if (values.contains("spacing"))
            layout->setSpacing(px(values.value("spacing").toInt()));
        const auto spacers = values.value("spacers").toMap();
        // Look up only live items: rebuilding a list may already have deleted old spacers.
        for (int index = 0; index < layout->count(); ++index) {
            auto* spacer = layout->itemAt(index)->spacerItem();
            const auto key = QString::number(reinterpret_cast<quintptr>(spacer));
            if (!spacer || !spacers.contains(key))
                continue;
            const auto original = spacers.value(key).toSize();
            spacer->changeSize(px(original.width()), px(original.height()),
                               spacer->sizePolicy().horizontalPolicy(),
                               spacer->sizePolicy().verticalPolicy());
        }
        layout->invalidate();
    }
}
void bind(QObject* object, const char* name, const QVariant& value) {
    auto values = object->property(metricsKey).toMap();
    values.insert(QLatin1String(name), value);
    object->setProperty(metricsKey, values);
    applyObject(object, basePixels(object));
}
} // namespace
int basePixels(const QObject* object) {
    for (auto* current = object; current; current = current->parent())
        if (current->property(zoomKey).isValid())
            return current->property(zoomKey).toInt();
    const int pixels = QApplication::font().pixelSize();
    return pixels > 0 ? InterfaceZoomMetrics::snap(pixels) : InterfaceZoomMetrics::mediumPixels;
}
void fixedHeight(QWidget* w, int n) { bind(w, "fixedHeight", n); }
void fixedWidth(QWidget* w, int n) { bind(w, "fixedWidth", n); }
void fixedSize(QWidget* w, const QSize& n) { bind(w, "fixedSize", n); }
void minimumHeight(QWidget* w, int n) { bind(w, "minimumHeight", n); }
void minimumWidth(QWidget* w, int n) { bind(w, "minimumWidth", n); }
void maximumWidth(QWidget* w, int n) { bind(w, "maximumWidth", n); }
void iconSize(QObject* w, const QSize& n) { bind(w, "iconSize", n); }
void margins(QObject* layout, int l, int t, int r, int b) {
    bind(layout, "margins", QVariantList{l, t, r, b});
}
void spacing(QObject* layout, int n) { bind(layout, "spacing", n); }
void horizontalSpacing(QLayout* layout, int n) { bind(layout, "horizontalSpacing", n); }
void verticalSpacing(QLayout* layout, int n) { bind(layout, "verticalSpacing", n); }
void addSpacing(QBoxLayout* layout, int n) {
    layout->addSpacing(n);
    auto* item = layout->itemAt(layout->count() - 1)->spacerItem();
    auto values = layout->property(metricsKey).toMap();
    auto spacers = values.value("spacers").toMap();
    // Discard registrations for removed items before adding another row.
    QSet<QString> live;
    for (int i = 0; i < layout->count(); ++i)
        live.insert(QString::number(reinterpret_cast<quintptr>(layout->itemAt(i)->spacerItem())));
    for (auto it = spacers.begin(); it != spacers.end();)
        if (!live.contains(it.key()))
            it = spacers.erase(it);
        else
            ++it;
    spacers.insert(QString::number(reinterpret_cast<quintptr>(item)), item->sizeHint());
    bind(layout, "spacers", spacers);
}
void apply(QWidget* surface, int base) {
    surface->setProperty(zoomKey, InterfaceZoomMetrics::snap(base));
    applyObject(surface, base);
    for (auto* child : surface->findChildren<QObject*>())
        if (child->property(metricsKey).isValid())
            applyObject(child, base);
}
} // namespace ketplus::chrome_geometry
