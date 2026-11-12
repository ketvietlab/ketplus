#include <QCoreApplication>

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

#include <cstdio>

// Exercise the real production delegate, without starting an updater or making requests.
@interface KetPlusSparkleDelegate : NSObject <SPUUpdaterDelegate>
- (instancetype)initWithOwner:(void*)owner;
@end

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    @autoreleasepool {
        NSBundle* bundle = [NSBundle mainBundle];
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
        if (previous)
            [defaults setPersistentDomain:previous forName:domain];
        else
            [defaults removePersistentDomainForName:domain];
        [defaults synchronize];
        std::fprintf(stdout,
                     "Compiled feed policy: %d/2 restored defaults ignored; no network checks\n",
                     2 - failures);
        return failures == 0 ? 0 : 2;
    }
}
