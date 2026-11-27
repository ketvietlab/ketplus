// Test instrumentation for a disposable Desktop bundle. Link separately from the product.
#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>
#import <objc/runtime.h>

#include <cstdio>
#include <cstdlib>
#include <limits.h>

namespace {

using Start = void (*)(SPUStandardUpdaterController*, SEL);
using Finish = void (*)(id, SEL, SPUUpdater*, SPUUpdateCheck, NSError*);
Start originalStart;
Finish originalFinish;
bool retryIssued = false;

void finish(id delegate, SEL selector, SPUUpdater* updater, SPUUpdateCheck check, NSError* error) {
    originalFinish(delegate, selector, updater, check, error);
    std::fprintf(stderr, "sparkle-qa cycle=%ld error=%s:%ld\n", static_cast<long>(check),
                 error != nil ? error.domain.UTF8String : "none", static_cast<long>(error.code));
    for (NSError* inner = error.userInfo[NSUnderlyingErrorKey]; inner != nil;
         inner = inner.userInfo[NSUnderlyingErrorKey]) {
        std::fprintf(stderr, "sparkle-qa underlying=%s:%ld\n", inner.domain.UTF8String,
                     static_cast<long>(inner.code));
    }
    const char* retry = std::getenv("KETPLUS_SPARKLE_QA_RETRY");
    if (!retryIssued && retry != nullptr && *retry == '1' && error.code == SUDownloadError) {
        retryIssued = true;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
          std::fputs("sparkle-qa retry check\n", stderr);
          [updater checkForUpdatesInBackground];
        });
    }
}

void found(id, SEL, SPUUpdater*, SUAppcastItem*) {
    std::fputs("sparkle-qa update offered\n", stderr);
}

BOOL ready(id, SEL, SPUUpdater*, SUAppcastItem*, void (^)(void)) {
    std::fputs("sparkle-qa install on quit ready\n", stderr);
    return NO;
}

void start(SPUStandardUpdaterController* controller, SEL selector) {
    originalStart(controller, selector);
    SPUUpdater* updater = controller.updater;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
      std::fprintf(stderr, "sparkle-qa check downloads=%d in-progress=%d\n",
                   updater.automaticallyDownloadsUpdates, updater.sessionInProgress);
      [updater checkForUpdatesInBackground];
    });
}

NSString* canonicalPath(NSString* path) {
    char resolved[PATH_MAX];
    if (!path.isAbsolutePath || realpath(path.fileSystemRepresentation, resolved) == nullptr)
        return nil;
    return [NSString stringWithUTF8String:resolved];
}

bool isChildOf(NSString* path, NSString* root) {
    return root != nil && [canonicalPath(path) hasPrefix:[root stringByAppendingString:@"/"]];
}

} // namespace

__attribute__((constructor)) static void installSparkleQaTrigger() {
    @autoreleasepool {
        NSBundle* bundle = NSBundle.mainBundle;
        const char* rawRoot = std::getenv("KETPLUS_SPARKLE_QA_ROOT");
        const char* rawSettings = std::getenv("KETPLUS_QA_SETTINGS_DIR");
        NSString* root =
            rawRoot == nullptr ? nil : canonicalPath([NSString stringWithUTF8String:rawRoot]);
        NSString* settings =
            rawSettings == nullptr ? nil : [NSString stringWithUTF8String:rawSettings];
        NSString* feed = [bundle objectForInfoDictionaryKey:@"SUFeedURL"];
        NSURL* feedUrl = [NSURL URLWithString:feed ?: @""];
        const bool qaIdentity = [bundle.bundleIdentifier hasPrefix:@"dev.ketplus.audit."];
        const bool scratchBundle = isChildOf(bundle.bundlePath, root);
        const bool scratchSettings = isChildOf(settings, root);
        const bool loopbackFeed = [feedUrl.scheme isEqualToString:@"http"] &&
                                  [feedUrl.host isEqualToString:@"127.0.0.1"] &&
                                  feedUrl.port != nil;
        if (!qaIdentity || !scratchBundle || !scratchSettings || !loopbackFeed) {
            std::fprintf(stderr,
                         "sparkle-qa rejected launch identity=%d bundle=%d settings=%d feed=%d\n",
                         qaIdentity, scratchBundle, scratchSettings, loopbackFeed);
            std::_Exit(125);
        }
        // Seed only this validated QA bundle's domain. A recent check keeps Sparkle's
        // scheduler idle while the trigger requests one background session.
        [NSUserDefaults.standardUserDefaults setPersistentDomain:@{
            @"SUEnableAutomaticChecks" : @YES,
            @"SUAutomaticallyUpdate" : @YES,
            @"SUHasLaunchedBefore" : @YES,
            @"SULastCheckTime" : NSDate.date,
        }
                                                         forName:bundle.bundleIdentifier];
        Method startMethod =
            class_getInstanceMethod(SPUStandardUpdaterController.class, @selector(startUpdater));
        Class delegate = NSClassFromString(@"KetPlusSparkleDelegate");
        Method finishMethod = class_getInstanceMethod(
            delegate, @selector(updater:didFinishUpdateCycleForUpdateCheck:error:));
        if (startMethod == nullptr || finishMethod == nullptr) {
            std::fputs("sparkle-qa cannot observe the Desktop updater\n", stderr);
            std::_Exit(125);
        }
        originalStart = reinterpret_cast<Start>(
            method_setImplementation(startMethod, reinterpret_cast<IMP>(start)));
        originalFinish = reinterpret_cast<Finish>(
            method_setImplementation(finishMethod, reinterpret_cast<IMP>(finish)));
        class_addMethod(delegate, @selector(updater:didFindValidUpdate:),
                        reinterpret_cast<IMP>(found), "v@:@@");
        class_addMethod(delegate,
                        @selector(updater:willInstallUpdateOnQuit:immediateInstallationBlock:),
                        reinterpret_cast<IMP>(ready), "B@:@@@?");
    }
}
