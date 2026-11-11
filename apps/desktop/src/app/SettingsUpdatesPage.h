#pragma once

#include <QWidget>

class QAbstractButton;
class QLabel;

namespace ket {
class Badge;
class UpdateCard;
} // namespace ket

namespace ketplus {

class AppUpdater;

// Settings → Updates as in the mock (ADR 0019): what the updater knows about this version, a
// ready update with Restart now and Later, automatic checks and downloads, and a manual check.
// Sparkle's own windows show release notes, download progress and the install prompt; this
// page never duplicates them. The mock's Channel select is omitted: the feed has a single channel.
class SettingsUpdatesPage final : public QWidget {
    Q_OBJECT

  public:
    explicit SettingsUpdatesPage(AppUpdater& updater, QWidget* parent = nullptr);

  private:
    void refresh();

    AppUpdater& updater_;
    QLabel* intro_{nullptr};
    ket::UpdateCard* card_{nullptr};
    ket::Badge* badge_{nullptr};
    QAbstractButton* restart_{nullptr};
    QAbstractButton* later_{nullptr};
    QAbstractButton* check_{nullptr};
    QAbstractButton* automaticChecks_{nullptr};
    QAbstractButton* automaticDownloads_{nullptr};
    QString laterVersion_; // Later hides the actions for this downloaded version
};

} // namespace ketplus
