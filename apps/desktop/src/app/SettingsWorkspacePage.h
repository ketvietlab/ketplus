#pragma once

#include "app/SettingsProvidersPage.h"
#include "ui/Theme.h"

#include <QWidget>

class QListWidget;

namespace ketplus {

class SettingsDialog;

class SettingsWorkspacePage final : public QWidget {
    Q_OBJECT

  public:
    explicit SettingsWorkspacePage(SettingsDialog* dialog,
                                   SettingsProvidersPage::Fetcher providers = {},
                                   QWidget* parent = nullptr,
                                   SettingsClaudeCredentials::Requester credentials = {});

    [[nodiscard]] SettingsDialog* dialog() const noexcept;
    void applyPalette(const ThemePalette& palette, int interfaceFontSizePixels = 14);
    // Selects a settings section by its navigation id, such as "updates".
    void showSection(const QString& id);

  signals:
    void saved();
    void dismissed();
    void accountDevicesRequested();
    void agentModelsRequested();

  private:
    SettingsDialog* dialog_{nullptr};
    QListWidget* navigation_{nullptr};
    ThemePalette palette_{};
    int interfaceFontSizePixels_{14};
};

} // namespace ketplus
