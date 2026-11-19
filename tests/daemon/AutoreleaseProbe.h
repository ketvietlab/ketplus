#pragma once

// Autoreleased Objective-C objects that count their own release, for tests that check an
// autorelease pool was drained. macOS only.
namespace ketplus::test {

void autoreleaseProbe();
int releasedProbes();
void resetReleasedProbes();

} // namespace ketplus::test
