#pragma once

#include "app/SettingsDialog.h"

#include <QVariantList>

#include <functional>

class QVBoxLayout;

namespace ket {
class EmptyState;
} // namespace ket

namespace ketplus {

// Settings → Archived as in the mock: one card per archived conversation or worktree with
// its number, folder and archive date, Restore and Delete…, and Stop turn while it runs.
class ArchivedSettingsPage final : public SettingsPage {
    Q_OBJECT

  public:
    using RestoreConversation = std::function<bool(const QString&, const QString&)>;
    using StopConversation = std::function<bool(const QString&, const QString&)>;
    // Asks to delete a conversation (it confirms first); the list follows setConversations.
    using DeleteConversation = std::function<void(const QString&, const QString&)>;

    explicit ArchivedSettingsPage(QVariantList conversations, RestoreConversation restore,
                                  StopConversation stop, DeleteConversation remove = {},
                                  QWidget* parent = nullptr);
    void setConversations(QVariantList conversations);

  private:
    void refresh();

    QVariantList conversations_;
    RestoreConversation restore_;
    StopConversation stop_;
    DeleteConversation delete_;
    ket::EmptyState* emptyState_{nullptr};
    QVBoxLayout* cards_{nullptr};
};

} // namespace ketplus
