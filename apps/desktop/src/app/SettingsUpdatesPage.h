#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;

namespace ketplus {

class AppUpdater;

// Settings → Updates (ADR 0019): the installed version, the last check, a manual check and the
// two automatic-update preferences. Sparkle's own windows show release notes, download progress
// and the install prompt; this page never duplicates them.
class SettingsUpdatesPage final : public QWidget {
    Q_OBJECT

  public:
    explicit SettingsUpdatesPage(AppUpdater& updater, QWidget* parent = nullptr);

  private:
    void refresh();

    AppUpdater& updater_;
    QLabel* status_{nullptr};
    QPushButton* check_{nullptr};
    QCheckBox* automaticChecks_{nullptr};
    QCheckBox* automaticDownloads_{nullptr};
};

} // namespace ketplus
