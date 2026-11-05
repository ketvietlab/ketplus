#pragma once

#include <QObject>
#include <QStringList>

#include <functional>

class QFileSystemWatcher;
class QTimer;

namespace ketplus::browser_tools {

// Applies browser tool settings to a running Codex app-server without restarting KetPlus
// (ADR 0008). The owner reports each launch with the arguments it used; a settings change
// (file watcher or notifySettingsChanged()) is debounced, the effective arguments are recomputed,
// and restartRequested() is emitted only when they differ from the running process and the owner
// is idle (no active turn, no in-flight call). A busy owner defers until it calls idleChanged().
class CodexLaunchReloader final : public QObject {
    Q_OBJECT

  public:
    static constexpr int defaultDebounceMs = 400;

    CodexLaunchReloader(std::function<QStringList()> computeArguments, std::function<bool()> isIdle,
                        QString settingsPath, int debounceMs = defaultDebounceMs,
                        QObject* parent = nullptr);

    // The process started with these (browser) arguments.
    void launched(const QStringList& arguments);
    // The process is gone; the next launch reads settings anyway.
    void stopped();
    // Settings may have changed: (re)starts the debounce timer.
    void settingsChanged();
    // The owner's idle state may have changed: applies a deferred restart when idle.
    void idleChanged();

    [[nodiscard]] bool restartPending() const noexcept { return pending_; }
    [[nodiscard]] bool running() const noexcept { return running_; }

  signals:
    void restartRequested();

  private:
    void evaluate();
    void watchSettingsFile();

    std::function<QStringList()> computeArguments_;
    std::function<bool()> isIdle_;
    QString settingsPath_;
    QStringList applied_;
    QTimer* debounce_{nullptr};
    QFileSystemWatcher* watcher_{nullptr};
    bool running_{false};
    bool pending_{false};
    bool evaluating_{false};
};

} // namespace ketplus::browser_tools
