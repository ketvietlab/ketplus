#pragma once

#include "app/SettingsAccountPage.h"
#include "app/SettingsProvidersPage.h"
#include "app/SettingsStoragePage.h"
#include "app/SettingsWorkspace.h"

namespace ketplus {

class SettingsDialog;

// Pro's Settings: the shared page (core SettingsWorkspace) with Conversation width on
// Typography and Pro's sections: Account & devices, Agent models, Browser, Computer use,
// Providers, Archived, Storage and Updates.
class SettingsWorkspacePage final : public SettingsWorkspace {
    Q_OBJECT

  public:
    explicit SettingsWorkspacePage(SettingsDialog* dialog,
                                   SettingsProvidersPage::Fetcher providers = {},
                                   QWidget* parent = nullptr,
                                   SettingsClaudeCredentials::Requester credentials = {},
                                   SettingsStoragePage::Requester storage = {});

    // Reads remote.status for Account & devices.
    void setAccountStatusReader(SettingsAccountPage::StatusReader reader) {
        account_->setStatusReader(std::move(reader));
    }
    void refreshAccount() { account_->refresh(); }

  signals:
    void accountDevicesRequested();
    void accountSignOutRequested();
    void agentModelsRequested();
    // A provider setup guide's command, to type into a terminal without running it (#170).
    void typeInTerminalRequested(const QString& command);

  private:
    void addConversationWidth();

    SettingsAccountPage* account_{nullptr};
};

} // namespace ketplus
