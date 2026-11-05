#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

namespace ketplus {

// ADR 0019: the Desktop's in-app updater. One instance serves the whole process, because the
// application bundle (not a window) is what gets replaced. The Sparkle implementation shows
// Sparkle's own release-notes, progress and install windows; this interface exposes only what
// Settings → Updates, the menu and the sidebar reminder need.
//
// Updating relaunches only the Desktop. Agent turns run in the per-user daemon, which keeps
// serving them; the new Desktop replaces that daemon once it is idle (ADR 0003).
class AppUpdater : public QObject {
    Q_OBJECT

  public:
    // The process-wide updater, created on first use. A build without a configured feed (or a
    // platform without Sparkle) gets an updater whose isConfigured() is false.
    static AppUpdater& instance();
    // Replaces the process-wide updater; the caller keeps ownership. Tests only.
    static void setInstanceForTesting(AppUpdater* updater);

    explicit AppUpdater(QObject* parent = nullptr);

    // Starts scheduled checks. Called once the first window is visible.
    virtual void start() {}
    [[nodiscard]] virtual bool isConfigured() const = 0;
    // False while a check, download or install session is already in progress.
    [[nodiscard]] virtual bool canCheckForUpdates() const = 0;
    // A user-initiated check: shows progress and the result, including "up to date".
    virtual void checkForUpdates() = 0;
    [[nodiscard]] virtual bool automaticallyChecksForUpdates() const = 0;
    virtual void setAutomaticallyChecksForUpdates(bool enabled) = 0;
    [[nodiscard]] virtual bool automaticallyDownloadsUpdates() const = 0;
    virtual void setAutomaticallyDownloadsUpdates(bool enabled) = 0;
    // Invalid until the first completed check.
    [[nodiscard]] virtual QDateTime lastUpdateCheck() const = 0;

    // A version a scheduled check found while KetPlus was not in focus. The sidebar shows a
    // reminder until the user opens it with checkForUpdates() or the session ends.
    [[nodiscard]] QString pendingVersion() const { return pendingVersion_; }

  signals:
    void stateChanged();
    void pendingVersionChanged(const QString& version);

  protected:
    void setPendingVersion(const QString& version);

  private:
    QString pendingVersion_;
};

} // namespace ketplus
