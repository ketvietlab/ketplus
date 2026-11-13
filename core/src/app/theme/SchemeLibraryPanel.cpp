#include "app/theme/SchemeLibraryPanel.h"
#include "KetAppearance.h"
#include "KetBadge.h"
#include "KetBanner.h"
#include "KetButton.h"
#include "KetChoice.h"
#include "KetEmptyState.h"
#include "KetTextField.h"
#include "ui/Theme.h"
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QThread>
#include <QVBoxLayout>
#include <memory>

namespace ketplus {
namespace {
// Content sample: the actual scheme values, never presentation colors for the controls.
class SchemeSwatch final : public QWidget {
  public:
    SchemeSwatch(const ColorScheme& scheme, QWidget* parent) : QWidget(parent) {
        const auto resolved = resolveColorScheme(scheme);
        colors_ = {resolved.editor.value(QStringLiteral("bg")),
                   resolved.editor.value(QStringLiteral("fg")),
                   resolved.syntax.value(QStringLiteral("keyword")).fg.value_or(QColor()),
                   resolved.syntax.value(QStringLiteral("string")).fg.value_or(QColor())};
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setToolTip(tr("Editor background, text, keyword and string colors"));
        setAccessibleName(toolTip());
        connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this, [this] {
            updateGeometry();
            update();
        });
    }
    QSize sizeHint() const override {
        const auto& a = ket::Appearance::instance();
        return {qRound(a.scaled(64)), qRound(a.scaled(24))};
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const qreal w = width() / qreal(colors_.size());
        for (qsizetype i = 0; i < colors_.size(); ++i)
            painter.fillRect(QRectF(i * w, 0, w, height()), colors_[i]);
    }

  private:
    QList<QColor> colors_;
};
} // namespace
SchemeLibraryPanel::SchemeLibraryPanel(QString directory, QWidget* parent, ThemeManager* theme)
    : QWidget(parent), library_(std::move(directory)), theme_(theme) {
    setObjectName(QStringLiteral("schemeLibrary"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    search_ = new ket::TextField(this);
    search_->setObjectName(QStringLiteral("schemeSearch"));
    search_->setPlaceholderText(tr("Search color schemes"));
    search_->setAccessibleName(tr("Search color schemes"));
    toolbar->addWidget(search_, 1);
    auto* import = new ket::Button(tr("Import…"), ket::ButtonVariant::Primary, this);
    import->setObjectName(QStringLiteral("importScheme"));
    toolbar->addWidget(import);
    layout->addLayout(toolbar);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumHeight(180);
    rows_ = new QWidget(scroll);
    rowLayout_ = new QVBoxLayout(rows_);
    rowLayout_->setContentsMargins(0, 0, 0, 0);
    rowLayout_->setSpacing(8);
    scroll->setWidget(rows_);
    layout->addWidget(scroll);
    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    export_ = new ket::Button(tr("Export…"), ket::ButtonVariant::Secondary, this);
    duplicate_ = new ket::Button(tr("Duplicate…"), ket::ButtonVariant::Secondary, this);
    remove_ = new ket::Button(tr("Delete…"), ket::ButtonVariant::Danger, this);
    export_->setObjectName(QStringLiteral("exportScheme"));
    duplicate_->setObjectName(QStringLiteral("duplicateScheme"));
    remove_->setObjectName(QStringLiteral("deleteScheme"));
    for (auto* button : {export_, duplicate_, remove_})
        actions->addWidget(button);
    actions->addStretch();
    layout->addLayout(actions);
    notice_ = new ket::Banner(ket::BannerTone::Danger, tr("Color scheme error"), {}, this);
    notice_->setObjectName(QStringLiteral("schemeError"));
    notice_->hide();
    layout->addWidget(notice_);
    connect(search_, &QLineEdit::textChanged, this, &SchemeLibraryPanel::rebuildRows);
    connect(import, &QAbstractButton::clicked, this, &SchemeLibraryPanel::chooseImport);
    connect(export_, &QAbstractButton::clicked, this, &SchemeLibraryPanel::exportSelected);
    connect(duplicate_, &QAbstractButton::clicked, this, &SchemeLibraryPanel::duplicateSelected);
    connect(remove_, &QAbstractButton::clicked, this, &SchemeLibraryPanel::deleteSelected);
    if (theme_) {
        preferences_ = new QWidget(this);
        layout->insertWidget(0, preferences_);
        connect(theme_, &ThemeManager::themeChanged, this, &SchemeLibraryPanel::syncSelection,
                Qt::QueuedConnection);

        connect(theme_, &ThemeManager::schemeFailed, this, &SchemeLibraryPanel::showError);
        connect(theme_, &ThemeManager::schemeFailed, this, &SchemeLibraryPanel::syncSelection,
                Qt::QueuedConnection);
        selectedId_ = theme_->schemeSelection().schemeId;
        connect(this, &SchemeLibraryPanel::previewRequested, theme_,
                &ThemeManager::previewColorScheme);
        connect(this, &SchemeLibraryPanel::previewFinished, theme_, &ThemeManager::cancelPreview);
        connect(this, &QObject::destroyed, theme_, &ThemeManager::cancelPreview);
    }
    const auto resizeMetrics = [layout, toolbar, actions, scroll, this] {
        const auto& appearance = ket::Appearance::instance();
        layout->setSpacing(qRound(appearance.scaled(12)));
        toolbar->setSpacing(qRound(appearance.scaled(8)));
        actions->setSpacing(qRound(appearance.scaled(8)));
        rowLayout_->setSpacing(qRound(appearance.scaled(8)));
        scroll->setMinimumHeight(qRound(appearance.scaled(180)));
    };
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this, resizeMetrics);
    resizeMetrics();
    reload();
    if (theme_)
        syncSelection();
}
void SchemeLibraryPanel::reload() {
    schemes_ = library_.catalog().list();
    bool found = false;
    for (const auto& scheme : schemes_)
        found |= scheme.id == selectedId_;
    if (!found)
        selectedId_ = schemes_.isEmpty() ? QString{} : schemes_.first().id;
    rebuildRows();
    rebuildPreferences();
}
void SchemeLibraryPanel::rebuildRows() {
    while (auto* item = rowLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto query = search_->text().trimmed();
    int visible = 0;
    for (const auto& scheme : schemes_) {
        const auto source = SchemeLibrary::sourceLabel(scheme);
        if (!query.isEmpty() && !scheme.name.contains(query, Qt::CaseInsensitive) &&
            !source.contains(query, Qt::CaseInsensitive))
            continue;
        auto* row = new QWidget(rows_);
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(8);
        auto* choice = new ket::Choice(scheme.name,
                                       scheme.kind == SchemeKind::Light  ? tr("Light")
                                       : scheme.kind == SchemeKind::Dark ? tr("Dark")
                                                                         : tr("High contrast"),
                                       ket::ToggleKind::Radio, row);
        choice->setObjectName(QStringLiteral("schemeRow"));
        choice->setAccessibleName(scheme.name + QStringLiteral(" · ") + source +
                                  QStringLiteral(" · ") + choice->description());
        choice->setProperty("schemeId", scheme.id);
        choice->setChecked(scheme.id == selectedId_);
        line->addWidget(choice, 1);
        line->addWidget(new SchemeSwatch(scheme, row));
        line->addWidget(new ket::Badge(source, ket::BadgeTone::Neutral, row));
        rowLayout_->addWidget(row);
        connect(choice, &QAbstractButton::clicked, this, [this, id = scheme.id] {
            if (!selectScheme(id))
                return;
            selectedId_ = id;
            // Update check states without deleting the focused/activating control.
            for (auto* button : rows_->findChildren<ket::Choice*>())
                button->setChecked(button->property("schemeId").toString() == id);
            remove_->setEnabled(!SchemeLibrary::isBuiltinId(id));
            emit schemeActivated(id);
        });
        ++visible;
    }
    if (!visible)
        rowLayout_->addWidget(new ket::EmptyState(
            {}, tr("No matching schemes"),
            tr("Try another name or source, or import a color scheme."), rows_));
    rowLayout_->addStretch();
    export_->setEnabled(!selectedId_.isEmpty());
    duplicate_->setEnabled(!selectedId_.isEmpty());
    remove_->setEnabled(!selectedId_.isEmpty() && !SchemeLibrary::isBuiltinId(selectedId_));
}
void SchemeLibraryPanel::showError(const QString& message) {
    notice_->setTone(ket::BannerTone::Danger);
    notice_->setTitle(tr("Color scheme error"));
    notice_->setMessage(message);
    notice_->show();
}
void SchemeLibraryPanel::chooseImport() {
    const auto path = QFileDialog::getOpenFileName(
        this, tr("Import color scheme"), {},
        tr("Color schemes (*.json *.jsonc *.vsix *.icls *.jar);;All files (*)"));
    if (!path.isEmpty())
        inspectFile(path);
}
void SchemeLibraryPanel::inspectFile(const QString& path) {
    const auto generation = ++inspectionGeneration_;
    notice_->setTone(ket::BannerTone::Info);
    notice_->setTitle(tr("Reading color scheme…"));
    notice_->setMessage(tr("Checking the file and preparing its import report."));
    notice_->show();
    auto result = std::make_shared<SchemeImportBatch>();
    auto* worker = QThread::create([path, result] { *result = SchemeLibrary::inspect(path); });
    connect(worker, &QThread::finished, this, [this, generation, result] {
        if (generation != inspectionGeneration_)
            return;
        notice_->hide();
        if (!result->ok()) {
            showError(result->diagnostics.isEmpty()
                          ? tr("No importable color schemes were found.")
                          : SchemeLibrary::diagnosticText(result->diagnostics));
            return;
        }
        if (result->candidates.size() > 1)
            chooseVariants(*result);
        else
            reviewCandidates(result->candidates);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}
} // namespace ketplus
