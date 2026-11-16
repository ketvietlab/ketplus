#include "cloud/KeychainCredentialBackend.h"
#include <CoreFoundation/CoreFoundation.h>
#include <mutex>

namespace ketplus::daemon {
namespace {
template <class T> class Owned {
  public:
    T value;
    explicit Owned(T value) : value(value) {}
    ~Owned() {
        if (value)
            CFRelease(value);
    }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
};
CFStringRef string(const QString& value) {
    const auto bytes = value.toUtf8();
    return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(bytes.constData()),
                                   bytes.size(), kCFStringEncodingUTF8, false);
}
bool failed(OSStatus status, QString* error) {
    if (status == errSecSuccess)
        return false;
    if (error)
        *error =
            QStringLiteral(
                "macOS Keychain is unavailable or access was denied (%1). Unlock the keychain and "
                "authorize the installed application, then retry. No plaintext fallback is used.")
                .arg(status);
    return true;
}
// Legacy login-keychain ACL prompts do not honor kSecUseAuthenticationUIFail on
// every macOS release. Serialize this scoped process setting and restore it after
// each synchronous operation; never leave interactive authorization enabled here.
class InteractionGuard {
    static std::mutex mutex_;
    std::lock_guard<std::mutex> lock_{mutex_};
    Boolean previous_{false};
    bool changed_{false};

  public:
    OSStatus status;
    InteractionGuard() : status(SecKeychainGetUserInteractionAllowed(&previous_)) {
        if (status == errSecSuccess) {
            status = SecKeychainSetUserInteractionAllowed(false);
            changed_ = status == errSecSuccess;
        }
    }
    ~InteractionGuard() {
        if (changed_)
            SecKeychainSetUserInteractionAllowed(previous_);
    }
};
std::mutex InteractionGuard::mutex_;
class Keychain final : public CredentialBackend {
    SecKeychainRef explicitKeychain_;
    bool available(SecKeychainRef* keychain, QString* error) {
        if (explicitKeychain_) {
            *keychain = explicitKeychain_;
            CFRetain(*keychain);
        } else if (failed(SecKeychainCopyDefault(keychain), error))
            return false;
        SecKeychainStatus status = 0;
        const auto result = SecKeychainGetStatus(*keychain, &status);
        if (failed(result, error) || !(status & kSecUnlockStateStatus)) {
            if (result == errSecSuccess)
                failed(errSecInteractionNotAllowed, error);
            CFRelease(*keychain);
            *keychain = nullptr;
            return false;
        }
        return true;
    }
    CFMutableDictionaryRef query(const QString& service, SecKeychainRef keychain) {
        auto values = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
        Owned name(string(QStringLiteral("dev.ketplus.credentials.v1.") + service));
        CFDictionarySetValue(values, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(values, kSecAttrService, name.value);
        CFDictionarySetValue(values, kSecAttrAccount, CFSTR("accounts"));
        CFDictionarySetValue(values, kSecAttrSynchronizable, kCFBooleanFalse);
        CFDictionarySetValue(values, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
        const void* keychains[] = {keychain};
        Owned list(CFArrayCreate(nullptr, keychains, 1, &kCFTypeArrayCallBacks));
        CFDictionarySetValue(values, kSecMatchSearchList, list.value);
        return values;
    }

  public:
    explicit Keychain(SecKeychainRef keychain) : explicitKeychain_(keychain) {
        if (explicitKeychain_)
            CFRetain(explicitKeychain_);
    }
    ~Keychain() override {
        if (explicitKeychain_)
            CFRelease(explicitKeychain_);
    }
    Result read(const QString& service, QByteArray* value, QString* error) override {
        value->clear();
        InteractionGuard interaction;
        if (failed(interaction.status, error))
            return Result::Error;
        SecKeychainRef keychain = nullptr;
        if (!available(&keychain, error))
            return Result::Error;
        Owned ownedKeychain(keychain);
        Owned values(query(service, keychain));
        CFDictionarySetValue(values.value, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(values.value, kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef result = nullptr;
        const auto status = SecItemCopyMatching(values.value, &result);
        Owned ownedResult(result);
        if (status == errSecItemNotFound)
            return Result::Missing;
        if (failed(status, error))
            return Result::Error;
        if (!result || CFGetTypeID(result) != CFDataGetTypeID()) {
            failed(errSecDecode, error);
            return Result::Error;
        }
        const auto data = static_cast<CFDataRef>(result);
        *value = QByteArray(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                            CFDataGetLength(data));
        return Result::Found;
    }
    bool write(const QString& service, const QByteArray& value, QString* error) override {
        InteractionGuard interaction;
        if (failed(interaction.status, error))
            return false;
        SecKeychainRef keychain = nullptr;
        if (!available(&keychain, error))
            return false;
        Owned ownedKeychain(keychain);
        Owned values(query(service, keychain));
        Owned data(
            CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(value.constData()), value.size()));
        const void* keys[] = {kSecValueData};
        const void* contents[] = {data.value};
        Owned update(CFDictionaryCreate(nullptr, keys, contents, 1, &kCFTypeDictionaryKeyCallBacks,
                                        &kCFTypeDictionaryValueCallBacks));
        auto status = SecItemUpdate(values.value, update.value);
        if (status == errSecItemNotFound) {
            CFDictionaryRemoveValue(values.value, kSecMatchSearchList);
            CFDictionarySetValue(values.value, kSecUseKeychain, keychain);
            CFDictionarySetValue(values.value, kSecValueData, data.value);
            // Default access binds the item to this signed application's requirement.
            status = SecItemAdd(values.value, nullptr);
        }
        return !failed(status, error);
    }
};
} // namespace
std::shared_ptr<CredentialBackend> keychainCredentialBackend(SecKeychainRef keychain) {
    return std::make_shared<Keychain>(keychain);
}
std::shared_ptr<CredentialBackend> platformCredentialBackend() {
    return keychainCredentialBackend();
}
} // namespace ketplus::daemon
