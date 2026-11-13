#include "app/theme/SchemeQuickPicker.h"
#include "KetButton.h"
#include "KetCommandPalette.h"
#include "KetDialog.h"
#include "app/theme/SchemeLibrary.h"
#include "ui/Theme.h"
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>

namespace ketplus {
SchemeQuickPicker::SchemeQuickPicker(ThemeManager& theme, QWidget* anchor, QString directory)
    : QObject(anchor->window()), theme_(&theme), palette_(new ket::CommandPalette(anchor)),
      schemes_(ColorSchemeCatalog(std::move(directory)).list()), initial_(theme.schemeSelection()) {
    setObjectName(QStringLiteral("schemePickerController"));
    palette_->setObjectName(QStringLiteral("schemeQuickPicker"));
    palette_->setPlaceholderText(tr("Choose a color scheme"));
    palette_->setEmptyText(tr("No matching schemes"), tr("Search by name or source."));
    palette_->searchField()->installEventFilter(this);
    connect(palette_, &ket::CommandPalette::queryChanged, this, &SchemeQuickPicker::filter);
    connect(palette_, &ket::CommandPalette::activated, this,
            [this](const ket::CommandItem& item) { activate(item.id); });
    // The kit closes before emitting activated. Defer cancellation until an
    // activation in the same event has had a chance to commit.
    connect(palette_, &ket::CommandPalette::closed, this, [this] {
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (theme_ && !committed_)
                    theme_->cancelPreview();
                deleteLater();
            },
            Qt::QueuedConnection);
    });
    connect(&theme, &ThemeManager::themeChanged, this, [this] {
        if (theme_ && !committed_ && theme_->schemeSelection() != initial_)
            palette_->dismiss();
    });
}
SchemeQuickPicker::~SchemeQuickPicker() {
    if (theme_ && !committed_)
        theme_->cancelPreview();
    delete palette_.data();
}
ket::CommandPalette* SchemeQuickPicker::palette() const { return palette_; }
void SchemeQuickPicker::open() {
    filter({});
    palette_->open();
    const auto active = initial_.followSystem ? theme_->colorScheme().id : initial_.schemeId;
    const auto index = filteredIds_.indexOf(active);
    if (index >= 0)
        palette_->setHighlightedIndex(int(index));
}
void SchemeQuickPicker::filter(const QString& query) {
    ket::CommandGroup group{tr("Color schemes"), {}};
    filteredIds_.clear();
    for (const auto& scheme : schemes_) {
        const auto source = SchemeLibrary::sourceLabel(scheme);
        if (!query.trimmed().isEmpty() &&
            !scheme.name.contains(query.trimmed(), Qt::CaseInsensitive) &&
            !source.contains(query.trimmed(), Qt::CaseInsensitive))
            continue;
        const auto kind = scheme.kind == SchemeKind::Light  ? tr("Light")
                          : scheme.kind == SchemeKind::Dark ? tr("Dark")
                                                            : tr("High contrast");
        group.items.append({scheme.id, scheme.name, kind + QStringLiteral(" · ") + source, {}});
        filteredIds_.append(scheme.id);
    }
    palette_->setGroups({group});
    palette_->setHighlightedIndex(0);
    if (palette_->isOpen())
        previewHighlighted();
}
void SchemeQuickPicker::previewHighlighted() {
    const int index = palette_->highlightedIndex();
    if (!theme_ || index < 0 || index >= filteredIds_.size())
        return;
    auto wanted = theme_->schemeSelection();
    wanted.followSystem = false;
    wanted.schemeId = filteredIds_[index];
    theme_->previewSchemeSelection(wanted);
}
bool SchemeQuickPicker::eventFilter(QObject* watched, QEvent* event) {
    if (watched == palette_->searchField() && event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Down || key == Qt::Key_Up) {
            palette_->setHighlightedIndex(palette_->highlightedIndex() +
                                          (key == Qt::Key_Down ? 1 : -1));
            previewHighlighted();
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}
void SchemeQuickPicker::activate(const QString& id) {
    if (!theme_)
        return;
    auto wanted = theme_->schemeSelection();
    wanted.followSystem = false;
    wanted.schemeId = id;
    theme_->previewSchemeSelection(wanted);
    committed_ = theme_->commitSchemePreview();
    if (!committed_) {
        auto* error =
            new ket::Dialog(tr("Could not select color scheme"), palette_->parentWidget());
        auto* text = new QLabel(theme_->schemeError());
        text->setTextFormat(Qt::PlainText);
        text->setWordWrap(true);
        error->setBody(text);
        auto* close = error->addButton(tr("OK"), ket::ButtonVariant::Primary);
        connect(close, &QAbstractButton::clicked, error, &ket::Dialog::accept);
        connect(error, &ket::Dialog::finished, error, &QObject::deleteLater);
        error->open();
    }
}
void showColorSchemePicker(ThemeManager& theme, QWidget* anchor) {
    if (auto* existing = anchor->window()->findChild<SchemeQuickPicker*>(
            QStringLiteral("schemePickerController"))) {
        if (existing->palette()->isOpen()) {
            existing->palette()->raise();
            return;
        }
        delete existing;
    }
    (new SchemeQuickPicker(theme, anchor))->open();
}
} // namespace ketplus
