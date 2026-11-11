#pragma once

#include "CodexBrowserTools.h"

#include <QWidget>

#include <functional>

class QAbstractButton;
class QLabel;
class QLineEdit;

namespace ketplus {

// Settings → Browser: KetPlus browser tools for agents (ADR 0008), on by default with all
// origins allowed. Values are written straight to the shared browser-tools.json; after each save
// running Codex processes are told to reload (they restart when idle), so no app restart is needed.
class SettingsBrowserPage final : public QWidget {
    Q_OBJECT

  public:
    explicit SettingsBrowserPage(QString settingsPath = browser_tools::settingsFilePath(),
                                 QString searchStart = {},
                                 browser_tools::Probes probes = browser_tools::systemProbes(),
                                 QWidget* parent = nullptr);

    // Non-blocking: shows a cached result (or "Checking…") at once and runs detection on the
    // global thread pool. Results from an older refresh are dropped.
    void refresh();
    [[nodiscard]] const browser_tools::Prerequisites& prerequisites() const noexcept {
        return prerequisites_;
    }
    // "bundled", "source-checkout" or empty when no runtime resolves.
    [[nodiscard]] QString runtimeSource() const { return runtimeSource_; }
    [[nodiscard]] bool checking() const noexcept { return checking_; }

  signals:
    void prerequisitesChecked();

  private:
    struct Check final {
        browser_tools::Prerequisites prerequisites;
        QString source;
    };
    void applyCheck(const Check& check);
    void setEnabledByUser(bool enabled);
    void setAllOriginsByUser(bool all);
    void saveOriginsByUser();
    void saveAppPathByUser();
    void updateOriginControls();
    void setStatus(const QString& text);
    // Re-reads browser-tools.json, applies only the edited fields and writes it back, so a
    // refresh or another writer's fields are never overwritten with stale page state.
    bool saveFields(const std::function<void(browser_tools::Settings&)>& edit);

    QString settingsPath_;
    QString searchStart_;
    browser_tools::Probes probes_;
    browser_tools::Settings settings_;
    browser_tools::Prerequisites prerequisites_;
    QString runtimeSource_;
    quint64 generation_{0};
    bool checking_{false};
    QAbstractButton* enabled_{nullptr};
    QAbstractButton* allOrigins_{nullptr};
    QAbstractButton* onlyOrigins_{nullptr};
    QLineEdit* origins_{nullptr};
    QLineEdit* appPath_{nullptr};
    QLabel* status_{nullptr};
};

} // namespace ketplus
