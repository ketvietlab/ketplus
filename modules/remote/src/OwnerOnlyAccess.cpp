#include "OwnerOnlyAccess.h"

#if defined(Q_OS_WIN)
#include <QDir>
#include <QScopeGuard>
#include <qt_windows.h>

#include <aclapi.h>
#include <sddl.h>
#endif

namespace ketplus::remote {
#if defined(Q_OS_WIN)
bool restrictToCurrentUser(const QString& path, const bool directory, QString* error) {
    const auto fail = [error](const QString& reason) {
        if (error)
            *error = reason;
        return false;
    };
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return fail(QStringLiteral("Unable to read the current user."));
    const auto closeToken = qScopeGuard([token] { ::CloseHandle(token); });
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0)
        return fail(QStringLiteral("Unable to read the current user."));
    QByteArray buffer(static_cast<qsizetype>(needed), '\0');
    if (!::GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed))
        return fail(QStringLiteral("Unable to read the current user."));
    PSID sid = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
    LPWSTR sidText = nullptr;
    if (!::ConvertSidToStringSidW(sid, &sidText))
        return fail(QStringLiteral("Unable to read the current user."));
    const QString user = QString::fromWCharArray(sidText);
    ::LocalFree(sidText);

    // P: protected from inheritance; one allow-all entry for this user (object+container
    // inherit on a directory).
    const QString sddl = QStringLiteral("D:P(A;%1;FA;;;%2)")
                             .arg(directory ? QStringLiteral("OICI") : QString(), user);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &descriptor, nullptr))
        return fail(QStringLiteral("Unable to build the owner-only access list."));
    const auto freeDescriptor = qScopeGuard([descriptor] { ::LocalFree(descriptor); });
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL acl = nullptr;
    if (!::GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) || !present)
        return fail(QStringLiteral("Unable to build the owner-only access list."));
    const QString native = QDir::toNativeSeparators(path);
    const DWORD status = ::SetNamedSecurityInfoW(
        const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(native.utf16())), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl,
        nullptr);
    if (status != ERROR_SUCCESS)
        return fail(QStringLiteral("Unable to restrict access to the current user (error %1).")
                        .arg(status));
    return true;
}
#else
bool restrictToCurrentUser(const QString&, bool, QString*) { return true; }
#endif
} // namespace ketplus::remote
