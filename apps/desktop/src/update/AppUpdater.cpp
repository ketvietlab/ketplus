#include "update/AppUpdater.h"

#include <QCoreApplication>
#include <QPointer>

namespace ketplus {

AppUpdater* createPlatformAppUpdater(QObject* parent);
#ifdef KETPLUS_HAS_SPARKLE
bool platformAppUpdaterAllowedInQa();
#endif

namespace {

QPointer<AppUpdater>& currentInstance() {
    static QPointer<AppUpdater> updater;
    return updater;
}

bool qaSettingsIsolationEnabled = false;

bool updaterAllowed() {
    if (!qaSettingsIsolationEnabled)
        return true;
#ifdef KETPLUS_HAS_SPARKLE
    return platformAppUpdaterAllowedInQa();
#else
    return false;
#endif
}

} // namespace

// Used when no feed is compiled in, or on platforms without Sparkle: never contacts a server.
class DisabledAppUpdater final : public AppUpdater {
  public:
    using AppUpdater::AppUpdater;

    [[nodiscard]] bool isConfigured() const override { return false; }
    [[nodiscard]] bool canCheckForUpdates() const override { return false; }
    void checkForUpdates() override {}
    [[nodiscard]] bool automaticallyChecksForUpdates() const override { return false; }
    void setAutomaticallyChecksForUpdates(bool) override {}
    [[nodiscard]] bool automaticallyDownloadsUpdates() const override { return false; }
    void setAutomaticallyDownloadsUpdates(bool) override {}
    [[nodiscard]] QDateTime lastUpdateCheck() const override { return {}; }
};

#ifndef KETPLUS_HAS_SPARKLE
AppUpdater* createPlatformAppUpdater(QObject* parent) { return new DisabledAppUpdater(parent); }
#endif

AppUpdater::AppUpdater(QObject* parent) : QObject(parent) {}

void AppUpdater::enableQaSettingsIsolation() {
    Q_ASSERT(currentInstance().isNull());
    qaSettingsIsolationEnabled = true;
}

AppUpdater& AppUpdater::instance() {
    auto& updater = currentInstance();
    if (updater.isNull()) {
        auto* parent = QCoreApplication::instance();
        if (updaterAllowed())
            updater = createPlatformAppUpdater(parent);
        else
            updater = new DisabledAppUpdater(parent);
    }
    return *updater;
}

void AppUpdater::setInstanceForTesting(AppUpdater* updater) { currentInstance() = updater; }

void AppUpdater::setPendingVersion(const QString& version, const bool downloaded) {
    const bool ready = !version.isEmpty() && downloaded;
    if (pendingVersion_ == version && pendingDownloaded_ == ready)
        return;
    pendingVersion_ = version;
    pendingDownloaded_ = ready;
    emit pendingVersionChanged(version);
}

} // namespace ketplus
