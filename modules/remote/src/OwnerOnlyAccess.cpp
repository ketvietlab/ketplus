#include "OwnerOnlyAccess.h"

#include <QLocalSocket>

#if defined(Q_OS_WIN)
#include <QDir>
#include <QScopeGuard>
#include <qt_windows.h>

#include <aclapi.h>
#include <sddl.h>
#endif

namespace ketplus::remote {
#if defined(Q_OS_WIN)
namespace {
// The user SID of `token` in `buffer`; null when it cannot be read.
PSID tokenUser(HANDLE token, QByteArray* buffer) {
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0)
        return nullptr;
    buffer->resize(static_cast<qsizetype>(needed));
    if (!::GetTokenInformation(token, TokenUser, buffer->data(), needed, &needed))
        return nullptr;
    return reinterpret_cast<TOKEN_USER*>(buffer->data())->User.Sid;
}
} // namespace

bool serverRunsAsCurrentUser(QLocalSocket* socket, QString* error) {
    const auto refuse = [error](const QString& reason) {
        if (error)
            *error = reason;
        return false;
    };
    if (socket == nullptr || socket->socketDescriptor() == -1)
        return refuse(QStringLiteral("The local daemon connection has no pipe handle."));
    const auto pipe = reinterpret_cast<HANDLE>(socket->socketDescriptor());
    ULONG serverId = 0;
    if (!::GetNamedPipeServerProcessId(pipe, &serverId))
        return refuse(QStringLiteral("Unable to identify the process serving the local daemon."));
    HANDLE server = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, serverId);
    if (server == nullptr)
        return refuse(QStringLiteral("Unable to inspect the process serving the local daemon."));
    const auto closeServer = qScopeGuard([server] { ::CloseHandle(server); });
    HANDLE serverToken = nullptr;
    HANDLE ownToken = nullptr;
    if (!::OpenProcessToken(server, TOKEN_QUERY, &serverToken))
        return refuse(QStringLiteral("Unable to read the user of the local daemon process."));
    const auto closeServerToken = qScopeGuard([serverToken] { ::CloseHandle(serverToken); });
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &ownToken))
        return refuse(QStringLiteral("Unable to read the current user."));
    const auto closeOwnToken = qScopeGuard([ownToken] { ::CloseHandle(ownToken); });
    QByteArray serverBuffer;
    QByteArray ownBuffer;
    PSID serverSid = tokenUser(serverToken, &serverBuffer);
    PSID ownSid = tokenUser(ownToken, &ownBuffer);
    if (serverSid == nullptr || ownSid == nullptr || !::EqualSid(serverSid, ownSid))
        return refuse(QStringLiteral("The local daemon endpoint is served by another user; "
                                     "refusing to send the credential."));
    return true;
}

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
bool serverRunsAsCurrentUser(QLocalSocket*, QString*) { return true; }
#endif
} // namespace ketplus::remote
