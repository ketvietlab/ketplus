#include "AutoreleaseProbe.h"

#import <Foundation/Foundation.h>

#include <atomic>

namespace {
std::atomic<int> released{0};
} // namespace

// Compiled without ARC, so the probe is autoreleased explicitly, as framework code does.
@interface KetPlusReleaseProbe : NSObject
@end

@implementation KetPlusReleaseProbe
- (void)dealloc {
    ++released;
    [super dealloc];
}
@end

namespace ketplus::test {

void autoreleaseProbe() { [[[KetPlusReleaseProbe alloc] init] autorelease]; }

int releasedProbes() { return released.load(); }

void resetReleasedProbes() { released = 0; }

} // namespace ketplus::test
