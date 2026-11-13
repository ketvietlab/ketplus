#include "KetAppearance.h"
#include "KetButton.h"
#include "KetFieldRow.h"
#include "KetSelect.h"
#include "app/theme/SchemeLibraryPanel.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QStyleHints>

namespace ketplus {
namespace {
bool darkScheme(const ColorScheme& scheme) {
    if (scheme.kind != SchemeKind::HighContrast)
        return scheme.kind == SchemeKind::Dark;
    return resolveColorScheme(scheme).editor.value(QStringLiteral("bg")).lightnessF() < 0.5;
}
} // namespace
bool SchemeLibraryPanel::selectScheme(const QString& id) {
    if (!theme_)
        return true;
    auto next = theme_->schemeSelection();
    next.followSystem = false;
    next.schemeId = id;
    return theme_->setSchemeSelection(next);
}
void SchemeLibraryPanel::syncSelection() {
    if (!theme_ || theme_->hasSchemePreview())
        return;
    const auto state = theme_->schemeSelection();
    selectedId_ = state.followSystem ? (qApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark
                                            ? state.darkSchemeId
                                            : state.lightSchemeId)
                                     : state.schemeId;
    for (auto* button : rows_->findChildren<QAbstractButton*>())
        button->setChecked(button->property("schemeId").toString() == selectedId_);
    remove_->setEnabled(!selectedId_.isEmpty() && !SchemeLibrary::isBuiltinId(selectedId_));
    for (const auto& key :
         {QStringLiteral("preferredLightScheme"), QStringLiteral("preferredDarkScheme")})
        if (auto* select = preferences_->findChild<ket::Select*>(key)) {
            const QSignalBlocker blocker(select);
            select->setCurrentIndex(select->findData(
                key == u"preferredDarkScheme" ? state.darkSchemeId : state.lightSchemeId));
        }
}
void SchemeLibraryPanel::rebuildPreferences() {
    if (!theme_ || !preferences_)
        return;
    auto* layout = qobject_cast<QHBoxLayout*>(preferences_->layout());
    if (!layout) {
        layout = new QHBoxLayout(preferences_);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
    }
    while (auto* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto state = theme_->schemeSelection();
    for (bool dark : {false, true}) {
        auto* select = new ket::Select(preferences_);
        select->setObjectName(dark ? QStringLiteral("preferredDarkScheme")
                                   : QStringLiteral("preferredLightScheme"));
        select->setAccessibleName(dark ? tr("Preferred dark scheme")
                                       : tr("Preferred light scheme"));
        for (const auto& scheme : schemes_)
            if (darkScheme(scheme) == dark)
                select->addItem(scheme.name, scheme.id);
        const auto selected = dark ? state.darkSchemeId : state.lightSchemeId;
        select->setCurrentIndex(select->findData(selected));
        auto* field = new ket::FieldRow(dark ? tr("Preferred dark") : tr("Preferred light"), select,
                                        {}, preferences_);
        field->setStacked(true);
        layout->addWidget(field);
        connect(select, &QComboBox::activated, this, [this, select, dark](int index) {
            auto next = theme_->schemeSelection();
            if (dark)
                next.darkSchemeId = select->itemData(index).toString();
            else
                next.lightSchemeId = select->itemData(index).toString();
            if (!theme_->setSchemeSelection(next))
                QMetaObject::invokeMethod(this, [this] { syncSelection(); }, Qt::QueuedConnection);
        });
    }
}
bool SchemeLibraryPanel::prepareRemoval(const ColorScheme& scheme) {
    if (!theme_)
        return true;
    auto next = theme_->schemeSelection();
    if (next.schemeId == scheme.id)
        next.schemeId =
            darkScheme(scheme) ? QStringLiteral("builtin-dark") : QStringLiteral("builtin-light");
    if (next.lightSchemeId == scheme.id)
        next.lightSchemeId = QStringLiteral("builtin-light");
    if (next.darkSchemeId == scheme.id)
        next.darkSchemeId = QStringLiteral("builtin-dark");
    return next == theme_->schemeSelection() || theme_->setSchemeSelection(next);
}
} // namespace ketplus
