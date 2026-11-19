#include "update/AppUpdater.h"

#include <QCoreApplication>
#include <QPointer>

namespace ketplus {

AppUpdater* createPlatformAppUpdater(QObject* parent);

namespace {

QPointer<AppUpdater>& currentInstance() {
    static QPointer<AppUpdater> updater;
    return updater;
}

bool updaterDisabledForQa = false;

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

void AppUpdater::disableForQa() {
    Q_ASSERT(currentInstance().isNull());
    updaterDisabledForQa = true;
}

AppUpdater& AppUpdater::instance() {
    auto& updater = currentInstance();
    if (updater.isNull()) {
        auto* parent = QCoreApplication::instance();
        if (updaterDisabledForQa)
            updater = new DisabledAppUpdater(parent);
        else
            updater = createPlatformAppUpdater(parent);
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
