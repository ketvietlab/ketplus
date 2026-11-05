#pragma once

#include "app/SettingsDialog.h"

#include <QVariantList>

#include <functional>

class QLabel;
class QListWidget;
class QPushButton;

namespace ketplus {

class ArchivedSettingsPage final : public SettingsPage {
    Q_OBJECT

  public:
    using RestoreConversation = std::function<bool(const QString&, const QString&)>;
    using StopConversation = std::function<bool(const QString&, const QString&)>;

    explicit ArchivedSettingsPage(QVariantList conversations, RestoreConversation restore,
                                  StopConversation stop, QWidget* parent = nullptr);

  private:
    void refresh();

    QVariantList conversations_;
    RestoreConversation restore_;
    StopConversation stop_;
    QLabel* emptyState_{nullptr};
    QListWidget* list_{nullptr};
    QPushButton* restoreButton_{nullptr};
    QPushButton* stopButton_{nullptr};
};

} // namespace ketplus
