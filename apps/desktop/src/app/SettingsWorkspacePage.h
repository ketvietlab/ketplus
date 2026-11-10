#pragma once

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

  signals:
    void accountDevicesRequested();
    void agentModelsRequested();

  private:
    void addConversationWidth();
};

} // namespace ketplus
