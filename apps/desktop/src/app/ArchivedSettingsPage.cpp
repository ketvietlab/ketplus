#include "app/ArchivedSettingsPage.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

namespace ketplus {

ArchivedSettingsPage::ArchivedSettingsPage(QVariantList conversations, RestoreConversation restore,
                                           StopConversation stop, QWidget* parent)
    : SettingsPage(QStringLiteral("archived"), QStringLiteral("Archived"), parent),
      conversations_(std::move(conversations)), restore_(std::move(restore)),
      stop_(std::move(stop)), emptyState_(new QLabel(this)), list_(new QListWidget(this)),
      restoreButton_(new QPushButton(QStringLiteral("Restore"), this)),
      stopButton_(new QPushButton(QStringLiteral("Stop turn"), this)) {
    restoreButton_->setObjectName(QStringLiteral("restoreArchivedItemButton"));
    stopButton_->setObjectName(QStringLiteral("stopArchivedTurnButton"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("Archived items"), this);
    title->setProperty("kvRole", QStringLiteral("settingsSectionTitle"));
    layout->addWidget(title);
    auto* description = new QLabel(
        QStringLiteral(
            "Archived worktrees and conversations stay on this Mac and can be restored here."),
        this);
    description->setWordWrap(true);
    description->setProperty("kvRole", QStringLiteral("settingsDescription"));
    layout->addWidget(description);
    emptyState_->setText(QStringLiteral("No archived items."));
    emptyState_->setAlignment(Qt::AlignCenter);
    list_->setObjectName(QStringLiteral("archivedConversationList"));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(emptyState_, 1);
    layout->addWidget(list_, 1);
    auto* actions = new QHBoxLayout();
    actions->addStretch();
    actions->addWidget(stopButton_);
    actions->addWidget(restoreButton_);
    layout->addLayout(actions);

    connect(list_, &QListWidget::currentRowChanged, this, [this](const int row) {
        restoreButton_->setEnabled(row >= 0);
        stopButton_->setEnabled(
            row >= 0 && conversations_.at(row).toMap().value(QStringLiteral("running")).toBool());
    });
    connect(stopButton_, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row < 0 || row >= conversations_.size()) {
            return;
        }
        const QVariantMap item = conversations_.at(row).toMap();
        if (stop_ && stop_(item.value(QStringLiteral("path")).toString(),
                           item.value(QStringLiteral("id")).toString())) {
            QVariantMap updated = item;
            updated.insert(QStringLiteral("running"), false);
            conversations_[row] = updated;
            refresh();
            list_->setCurrentRow(row);
        }
    });
    connect(restoreButton_, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row < 0 || row >= conversations_.size()) {
            return;
        }
        const QVariantMap item = conversations_.at(row).toMap();
        if (restore_ && restore_(item.value(QStringLiteral("path")).toString(),
                                 item.value(QStringLiteral("id")).toString())) {
            conversations_.removeAt(row);
            refresh();
        }
    });
    refresh();
}

void ArchivedSettingsPage::refresh() {
    list_->clear();
    for (const QVariant& value : std::as_const(conversations_)) {
        const QVariantMap item = value.toMap();
        const QString kind =
            item.value(QStringLiteral("kind"), QStringLiteral("conversation")).toString();
        QString label =
            QStringLiteral("%1  ·  %2").arg(item.value(QStringLiteral("title")).toString(), kind);
        if (item.value(QStringLiteral("running")).toBool()) {
            label.append(QStringLiteral("  ·  Running"));
        }
        auto* row = new QListWidgetItem(label, list_);
        row->setToolTip(item.value(QStringLiteral("path")).toString());
    }
    const bool empty = conversations_.isEmpty();
    emptyState_->setVisible(empty);
    list_->setVisible(!empty);
    restoreButton_->setVisible(!empty);
    stopButton_->setVisible(!empty);
    restoreButton_->setEnabled(!empty && list_->currentRow() >= 0);
    stopButton_->setEnabled(false);
}

} // namespace ketplus
