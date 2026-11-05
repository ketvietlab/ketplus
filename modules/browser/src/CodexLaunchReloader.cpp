#include "CodexLaunchReloader.h"

#include "CodexBrowserTools.h"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>

#include <utility>

namespace ketplus::browser_tools {
namespace {

class SettingsNotifier final : public QObject {
    Q_OBJECT

  signals:
    void changed();
};

SettingsNotifier* notifier() {
    static SettingsNotifier instance;
    return &instance;
}

} // namespace

void notifySettingsChanged() {
    auto* hub = notifier();
    emit hub->changed();
}

CodexLaunchReloader::CodexLaunchReloader(std::function<QStringList()> computeArguments,
                                         std::function<bool()> isIdle, QString settingsPath,
                                         const int debounceMs, QObject* parent)
    : QObject(parent), computeArguments_(std::move(computeArguments)), isIdle_(std::move(isIdle)),
      settingsPath_(std::move(settingsPath)), debounce_(new QTimer(this)) {
    debounce_->setSingleShot(true);
    debounce_->setInterval(debounceMs);
    connect(debounce_, &QTimer::timeout, this, &CodexLaunchReloader::evaluate);
    connect(notifier(), &SettingsNotifier::changed, this, &CodexLaunchReloader::settingsChanged);
    if (!settingsPath_.isEmpty()) {
        watcher_ = new QFileSystemWatcher(this);
        // QSaveFile replaces the file, which drops a file watch: watch the directory as well.
        connect(watcher_, &QFileSystemWatcher::fileChanged, this, [this] {
            watchSettingsFile();
            settingsChanged();
        });
        connect(watcher_, &QFileSystemWatcher::directoryChanged, this, [this] {
            watchSettingsFile();
            settingsChanged();
        });
        watchSettingsFile();
    }
}

void CodexLaunchReloader::watchSettingsFile() {
    const QString directory = QFileInfo(settingsPath_).absolutePath();
    if (QFileInfo::exists(directory) && !watcher_->directories().contains(directory))
        watcher_->addPath(directory);
    if (QFileInfo::exists(settingsPath_) && !watcher_->files().contains(settingsPath_))
        watcher_->addPath(settingsPath_);
}

void CodexLaunchReloader::launched(const QStringList& arguments) {
    applied_ = arguments;
    running_ = true;
    pending_ = false;
}

void CodexLaunchReloader::stopped() {
    running_ = false;
    pending_ = false;
}

void CodexLaunchReloader::settingsChanged() { debounce_->start(); }

void CodexLaunchReloader::idleChanged() {
    if (pending_ && !debounce_->isActive())
        evaluate();
}

void CodexLaunchReloader::evaluate() {
    if (evaluating_ || !running_) {
        pending_ = false;
        return;
    }
    evaluating_ = true;
    const QStringList arguments = computeArguments_ ? computeArguments_() : QStringList{};
    evaluating_ = false;
    if (arguments == applied_) {
        pending_ = false; // e.g. only the prerequisite cache was rewritten
        return;
    }
    if (isIdle_ && !isIdle_()) {
        pending_ = true;
        return;
    }
    // The owner stops the process; its next launch() records the new arguments, so a rewrite
    // that produces the same arguments again never triggers a second restart.
    pending_ = false;
    running_ = false;
    emit restartRequested();
}

} // namespace ketplus::browser_tools

#include "CodexLaunchReloader.moc"
