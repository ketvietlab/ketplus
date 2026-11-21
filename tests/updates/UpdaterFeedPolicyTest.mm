#include "update/AppUpdater.h"

#include <QCoreApplication>

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

#include <cstdio>
#include <cstdlib>

namespace ketplus {
bool sparkleQaBundleAllowed(NSString* bundleIdentifier);
}

// Exercise the real production delegate, without starting an updater or making requests.
@interface KetPlusSparkleDelegate : NSObject <SPUUpdaterDelegate>
- (instancetype)initWithOwner:(void*)owner;
@end

namespace {
int probeArchitecture(NSBundle* bundle, bool expectOffer) {
    NSString* domain = bundle.bundleIdentifier;
    NSURL* feed = [NSURL URLWithString:[bundle objectForInfoDictionaryKey:@"SUFeedURL"] ?: @""];
    if (![domain hasPrefix:@"dev.ketplus.tests.updater-architecture."] ||
        ![feed.scheme isEqualToString:@"http"] || ![feed.host isEqualToString:@"127.0.0.1"] ||
        feed.port == nil || std::getenv("KETPLUS_QA_SETTINGS_DIR") == nullptr)
        return 3;

    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    [defaults setPersistentDomain:@{@"SUEnableAutomaticChecks" : @NO} forName:domain];
    auto* delegate = [[KetPlusSparkleDelegate alloc] initWithOwner:nullptr];
    auto* userDriver = [[SPUStandardUserDriver alloc] initWithHostBundle:bundle delegate:nil];
    auto* updater = [[SPUUpdater alloc] initWithHostBundle:bundle
                                         applicationBundle:bundle
                                                userDriver:userDriver
                                                  delegate:delegate];
    __block bool offered = false;
    id observer = [NSNotificationCenter.defaultCenter
        addObserverForName:SUUpdaterDidFindValidUpdateNotification
                    object:updater
                     queue:nil
                usingBlock:^(NSNotification*) {
                  offered = true;
                }];
    NSError* error = nil;
    const bool started = [updater startUpdater:&error];
    if (started) {
        [updater checkForUpdateInformation];
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:10];
        while (updater.sessionInProgress && deadline.timeIntervalSinceNow > 0)
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    [NSNotificationCenter.defaultCenter removeObserver:observer];
    [defaults removePersistentDomainForName:domain];
    [defaults synchronize];
    std::fprintf(stdout, "Architecture probe: offered=%d started=%d finished=%d\n", offered,
                 started, !updater.sessionInProgress);
    if (!started)
        std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
    return started && !updater.sessionInProgress && offered == expectOffer ? 0 : 4;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    @autoreleasepool {
        NSBundle* bundle = [NSBundle mainBundle];
        if (app.arguments().size() == 2) {
            const auto option = app.arguments().at(1);
            if (option == QStringLiteral("--expect-offer") ||
                option == QStringLiteral("--expect-no-offer"))
                return probeArchitecture(bundle, option == QStringLiteral("--expect-offer"));
            return 3;
        }
        NSString* domain = @"dev.ketplus.tests.updater-feed";
        if (![bundle.bundleIdentifier isEqualToString:domain])
            return 1;
        NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
        NSDictionary* previous = [defaults persistentDomainForName:domain];
        auto* delegate = [[KetPlusSparkleDelegate alloc] initWithOwner:nullptr];
        int failures = 0;
        for (NSString* restored in @[
                 @"https://updates.example.invalid/staging/x86_64/appcast.xml",
                 @"http://127.0.0.1:4443/stale.xml"
             ]) {
            [defaults setPersistentDomain:@{@"SUFeedURL" : restored} forName:domain];
            [defaults synchronize];
            auto* controller =
                [[SPUStandardUpdaterController alloc] initWithStartingUpdater:NO
                                                              updaterDelegate:delegate
                                                           userDriverDelegate:nil];
            NSString* expected = [bundle objectForInfoDictionaryKey:@"SUFeedURL"];
            if (![controller.updater.feedURL.absoluteString isEqualToString:expected])
                ++failures;
        }
        const bool qaIdentityPolicy = !ketplus::sparkleQaBundleAllowed(@"dev.ketplus.editor") &&
                                      !ketplus::sparkleQaBundleAllowed(@"DEV.KETPLUS.EDITOR") &&
                                      !ketplus::sparkleQaBundleAllowed(nil) &&
                                      !ketplus::sparkleQaBundleAllowed(@"") &&
                                      ketplus::sparkleQaBundleAllowed(bundle.bundleIdentifier);
        ketplus::AppUpdater::enableQaSettingsIsolation();
        const bool qaUpdaterConfigured = ketplus::AppUpdater::instance().isConfigured();
        if (previous)
            [defaults setPersistentDomain:previous forName:domain];
        else
            [defaults removePersistentDomainForName:domain];
        [defaults synchronize];
        if (!qaIdentityPolicy) {
            std::fputs("Unexpected QA bundle identity policy\n", stderr);
            return 3;
        }
        if (!qaUpdaterConfigured) {
            std::fputs("QA bundle did not retain its Sparkle updater\n", stderr);
            return 4;
        }
        std::fprintf(stdout,
                     "Compiled feed policy: %d/2 restored defaults ignored; no network checks\n",
                     2 - failures);
        return failures == 0 ? 0 : 2;
    }
}
