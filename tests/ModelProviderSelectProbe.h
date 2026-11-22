#pragma once

// T-12: reads and drives the provider select of Model & context (ModelProviderSelect.qml) the
// way its entries behave, without opening the popup.
#include <QMetaObject>
#include <QQuickItem>
#include <QVariantMap>

namespace model_provider_select {

inline QQuickItem* row(QQuickItem* item) {
    if (!item)
        return nullptr;
    if (item->objectName() == QStringLiteral("providerSelectRow"))
        return item;
    for (QQuickItem* child : item->childItems())
        if (QQuickItem* found = row(child))
            return found;
    return nullptr;
}

// The entry {id, label, enabled, reason} of `provider`; empty when it is not listed.
inline QVariantMap choice(QQuickItem* select, const QString& provider) {
    if (!select)
        return {};
    for (const QVariant& value : select->property("choices").toList())
        if (value.toMap().value(QStringLiteral("id")).toString() == provider)
            return value.toMap();
    return {};
}

inline bool enabled(QQuickItem* select, const QString& provider) {
    return choice(select, provider).value(QStringLiteral("enabled")).toBool();
}

// Chooses `provider` as its entry does; false when it is disabled or not listed.
inline bool choose(QQuickItem* select, const QString& provider) {
    QVariant chosen;
    return select &&
           QMetaObject::invokeMethod(select, "choose", Q_RETURN_ARG(QVariant, chosen),
                                     Q_ARG(QVariant, provider)) &&
           chosen.toBool();
}

} // namespace model_provider_select
