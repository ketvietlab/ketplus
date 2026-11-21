#include <CoreFoundation/CoreFoundation.h>

namespace {

// Observe Qt's native key lookup on the marked test thread and supply a
// synthetic value before the lookup can contact cfprefsd.
thread_local int* nativeReads = nullptr;

CFPropertyListRef observeNativeValue(CFStringRef key, CFStringRef application, CFStringRef user,
                                     CFStringRef host) {
    if (!nativeReads)
        return CFPreferencesCopyValue(key, application, user, host);
    ++*nativeReads;
    return CFRetain(CFSTR("native-sentinel"));
}

__attribute__((used, section("__DATA,__interpose"))) const struct {
    decltype(&CFPreferencesCopyValue) replacement;
    decltype(&CFPreferencesCopyValue) original;
} nativeValueObserver{observeNativeValue, CFPreferencesCopyValue};

} // namespace

void observeQaNativeSettingsReads(int* count) { nativeReads = count; }
