#include "providers/ClaudeCredentials.h"
#include <QCryptographicHash>
#if defined(Q_OS_MACOS)
#include <Security/Security.h>
#endif

namespace ketplus::daemon {
ClaudeCredentials::Storage ClaudeCredentials::keychainStorage(const QString& profile) {
#if defined(Q_OS_MACOS)
    const QByteArray account =
        QCryptographicHash::hash(profile.toUtf8(), QCryptographicHash::Sha256).toHex();
    const auto query = [account] {
        auto dict = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(dict, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(dict, kSecAttrService, CFSTR("vn.ketviet.ketplus.claude-api"));
        auto name = CFStringCreateWithCString(nullptr, account.constData(), kCFStringEncodingUTF8);
        CFDictionarySetValue(dict, kSecAttrAccount, name);
        CFRelease(name);
        // A background daemon must fail visibly instead of waiting on a hidden Keychain dialog.
        CFDictionarySetValue(dict, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
        return dict;
    };
    const auto errorText = [](OSStatus code) {
        return QStringLiteral("Keychain access failed (%1). Unlock the login keychain and allow "
                              "the signed KetPlus daemon to access its Claude credential.")
            .arg(code);
    };
    return {[query, errorText](QString* error) {
                auto dict = query();
                CFDictionarySetValue(dict, kSecReturnData, kCFBooleanTrue);
                CFTypeRef result = nullptr;
                const OSStatus code = SecItemCopyMatching(dict, &result);
                CFRelease(dict);
                QByteArray data;
                if (code == errSecSuccess && result && CFGetTypeID(result) == CFDataGetTypeID()) {
                    auto bytes = static_cast<CFDataRef>(result);
                    data = QByteArray(reinterpret_cast<const char*>(CFDataGetBytePtr(bytes)),
                                      CFDataGetLength(bytes));
                } else if (code != errSecItemNotFound) {
                    *error = errorText(code);
                }
                if (result)
                    CFRelease(result);
                return data;
            },
            [query, errorText](const QByteArray& value, QString* error) {
                auto dict = query();
                auto data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(value.constData()),
                                         value.size());
                const void* keys[] = {kSecValueData};
                const void* values[] = {data};
                auto updates =
                    CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                       &kCFTypeDictionaryValueCallBacks);
                OSStatus code = SecItemUpdate(dict, updates);
                if (code == errSecItemNotFound) {
                    CFDictionarySetValue(dict, kSecValueData, data);
                    code = SecItemAdd(dict, nullptr);
                }
                CFRelease(updates);
                CFRelease(data);
                CFRelease(dict);
                if (code != errSecSuccess)
                    *error = errorText(code);
                return code == errSecSuccess;
            },
            QStringLiteral("macOS Keychain")};
#else
    Q_UNUSED(profile)
    return {[](QString*) { return QByteArray(); },
            [](const QByteArray&, QString* error) {
                *error =
                    QStringLiteral("Saving Claude API keys requires macOS Keychain in this "
                                   "release. Configure the Claude runtime on this host instead.");
                return false;
            },
            QStringLiteral("unavailable")};
#endif
}
} // namespace ketplus::daemon
