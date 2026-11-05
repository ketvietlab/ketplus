// Compiled with -fobjc-arc (see apps/desktop/CMakeLists.txt).
#include "update/AppUpdater.h"

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

#include <QMetaObject>
#include <QPointer>

namespace ketplus {
class SparkleAppUpdater;
}

// Sparkle calls delegates on the main thread, which is also Qt's GUI thread.
@interface KetPlusSparkleDelegate : NSObject <SPUUpdaterDelegate, SPUStandardUserDriverDelegate>
- (instancetype)initWithOwner:(ketplus::SparkleAppUpdater*)owner;
- (void)detach;
@end

namespace ketplus {

class SparkleAppUpdater final : public AppUpdater {
  public:
    explicit SparkleAppUpdater(QObject* parent) : AppUpdater(parent) {
        delegate_ = [[KetPlusSparkleDelegate alloc] initWithOwner:this];
        controller_ = [[SPUStandardUpdaterController alloc] initWithStartingUpdater:NO
                                                                    updaterDelegate:delegate_
                                                                 userDriverDelegate:delegate_];
        [controller_.updater addObserver:delegate_
                              forKeyPath:@"canCheckForUpdates"
                                 options:NSKeyValueObservingOptionNew
                                 context:nullptr];
    }

    ~SparkleAppUpdater() override {
        [controller_.updater removeObserver:delegate_ forKeyPath:@"canCheckForUpdates"];
        [delegate_ detach];
    }

    void start() override {
        if (started_)
            return;
        started_ = true;
        [controller_ startUpdater];
        emit stateChanged();
    }
    [[nodiscard]] bool isConfigured() const override { return true; }
    [[nodiscard]] bool canCheckForUpdates() const override {
        return started_ && controller_.updater.canCheckForUpdates;
    }
    void checkForUpdates() override {
        start();
        [controller_ checkForUpdates:nil];
    }
    [[nodiscard]] bool automaticallyChecksForUpdates() const override {
        return controller_.updater.automaticallyChecksForUpdates;
    }
    void setAutomaticallyChecksForUpdates(const bool enabled) override {
        controller_.updater.automaticallyChecksForUpdates = enabled;
        emit stateChanged();
    }
    [[nodiscard]] bool automaticallyDownloadsUpdates() const override {
        return controller_.updater.automaticallyDownloadsUpdates;
    }
    void setAutomaticallyDownloadsUpdates(const bool enabled) override {
        controller_.updater.automaticallyDownloadsUpdates = enabled;
        emit stateChanged();
    }
    [[nodiscard]] QDateTime lastUpdateCheck() const override {
        NSDate* date = controller_.updater.lastUpdateCheckDate;
        return date == nil ? QDateTime() : QDateTime::fromNSDate(date);
    }

    void notifyStateChanged() { emit stateChanged(); }
    void remindLater(const QString& version) { setPendingVersion(version); }

  private:
    KetPlusSparkleDelegate* delegate_{nil};
    SPUStandardUpdaterController* controller_{nil};
    bool started_{false};
};

AppUpdater* createPlatformAppUpdater(QObject* parent) { return new SparkleAppUpdater(parent); }

} // namespace ketplus

@implementation KetPlusSparkleDelegate {
    QPointer<ketplus::SparkleAppUpdater> _owner;
}

- (instancetype)initWithOwner:(ketplus::SparkleAppUpdater*)owner {
    if ((self = [super init]))
        _owner = owner;
    return self;
}

- (void)detach {
    _owner = nullptr;
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context {
    (void)keyPath;
    (void)object;
    (void)change;
    (void)context;
    // KVO may fire inside Sparkle's own state transitions; let it finish first.
    QPointer<ketplus::SparkleAppUpdater> owner = _owner;
    QMetaObject::invokeMethod(
        owner.data(),
        [owner] {
            if (owner)
                owner->notifyStateChanged();
        },
        Qt::QueuedConnection);
}

- (void)updater:(SPUUpdater*)updater
    didFinishUpdateCycleForUpdateCheck:(SPUUpdateCheck)updateCheck
                                 error:(NSError*)error {
    (void)updater;
    (void)updateCheck;
    (void)error;
    if (_owner)
        _owner->notifyStateChanged();
}

// Gentle reminders: a scheduled check never interrupts with a modal alert while the user is
// working (agent turns may be running). The sidebar shows "Update ready" instead.
- (BOOL)supportsGentleScheduledUpdateReminders {
    return YES;
}

- (BOOL)standardUserDriverShouldHandleShowingScheduledUpdate:(SUAppcastItem*)update
                                         andInImmediateFocus:(BOOL)immediateFocus {
    (void)update;
    return immediateFocus;
}

- (void)standardUserDriverWillHandleShowingUpdate:(BOOL)handleShowingUpdate
                                        forUpdate:(SUAppcastItem*)update
                                            state:(SPUUserUpdateState*)state {
    if (!_owner || handleShowingUpdate || state.userInitiated)
        return;
    _owner->remindLater(QString::fromNSString(update.displayVersionString));
}

- (void)standardUserDriverDidReceiveUserAttentionForUpdate:(SUAppcastItem*)update {
    (void)update;
    if (_owner)
        _owner->remindLater({});
}

- (void)standardUserDriverWillFinishUpdateSession {
    if (_owner) {
        _owner->remindLater({});
        _owner->notifyStateChanged();
    }
}

@end
