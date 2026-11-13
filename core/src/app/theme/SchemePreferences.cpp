#include "KetAppearance.h"
#include "KetButton.h"
#include "KetFieldRow.h"
#include "KetSelect.h"
#include "KetToggle.h"
#include "app/theme/SchemeLibraryPanel.h"
#include "ui/theme/ThemeSelection.h"
#include <QVBoxLayout>

namespace ketplus {
namespace {
bool darkScheme(const ColorScheme& scheme) {
    if (scheme.kind != SchemeKind::HighContrast)
        return scheme.kind == SchemeKind::Dark;
    return resolveColorScheme(scheme).editor.value(QStringLiteral("bg")).lightnessF() < 0.5;
}
} // namespace
bool SchemeLibraryPanel::selectScheme(const QString& id) {
    if (!selection_)
        return true;
    auto next = selection_->selection();
    next.followSystem = false;
    next.schemeId = id;
    return selection_->commit(next);
}
void SchemeLibraryPanel::syncSelection() {
    if (!selection_)
        return;
    const auto state = selection_->selection();
    selectedId_ = state.followSystem ? (ket::Appearance::instance().dark() ? state.darkSchemeId
                                                                           : state.lightSchemeId)
                                     : state.schemeId;
    for (auto* button : rows_->findChildren<QAbstractButton*>())
        button->setChecked(button->property("schemeId").toString() == selectedId_);
    remove_->setEnabled(!selectedId_.isEmpty() && !selectedId_.startsWith(u"builtin-"));
    rebuildPreferences();
}
void SchemeLibraryPanel::rebuildPreferences() {
    if (!selection_ || !preferences_)
        return;
    auto* layout = qobject_cast<QVBoxLayout*>(preferences_->layout());
    if (!layout) {
        layout = new QVBoxLayout(preferences_);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
    }
    while (auto* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto state = selection_->selection();
    auto* follow = new ket::Checkbox(tr("Follow system appearance"), preferences_);
    follow->setObjectName(QStringLiteral("schemeFollowSystem"));
    follow->setChecked(state.followSystem);
    layout->addWidget(follow);
    connect(follow, &QAbstractButton::toggled, this, [this](bool value) {
        auto next = selection_->selection();
        if (!value && next.followSystem)
            next.schemeId =
                ket::Appearance::instance().dark() ? next.darkSchemeId : next.lightSchemeId;
        next.followSystem = value;
        if (!selection_->commit(next))
            QMetaObject::invokeMethod(this, [this] { syncSelection(); }, Qt::QueuedConnection);
    });
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
        auto* field =
            new ket::FieldRow(dark ? tr("Dark scheme") : tr("Light scheme"), select,
                              tr("Used when following the system appearance."), preferences_);
        field->setStacked(true);
        layout->addWidget(field);
        connect(select, &QComboBox::activated, this, [this, select, dark](int index) {
            auto next = selection_->selection();
            if (dark)
                next.darkSchemeId = select->itemData(index).toString();
            else
                next.lightSchemeId = select->itemData(index).toString();
            if (!selection_->commit(next))
                QMetaObject::invokeMethod(this, [this] { syncSelection(); }, Qt::QueuedConnection);
        });
    }
}
bool SchemeLibraryPanel::prepareRemoval(const ColorScheme& scheme) {
    if (!selection_)
        return true;
    auto next = selection_->selection();
    if (next.schemeId == scheme.id)
        next.schemeId =
            darkScheme(scheme) ? QStringLiteral("builtin-dark") : QStringLiteral("builtin-light");
    if (next.lightSchemeId == scheme.id)
        next.lightSchemeId = QStringLiteral("builtin-light");
    if (next.darkSchemeId == scheme.id)
        next.darkSchemeId = QStringLiteral("builtin-dark");
    return next == selection_->selection() || selection_->commit(next);
}
} // namespace ketplus
