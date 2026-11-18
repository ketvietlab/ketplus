// Windows-only checks for what POSIX tests assert with mode bits and signals: the owner-only
// access list and the process-tree Job Object. Skipped on other hosts.
#include "OwnerOnlyAccess.h"
#include "platform/ProcessTree.h"

#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#if defined(Q_OS_WIN)
#include <qt_windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#endif

using namespace ketplus;

class WindowsPlatformTest final : public QObject {
    Q_OBJECT
  private slots:
    void directoryAccessListNamesOnlyTheCurrentUser() {
#if defined(Q_OS_WIN)
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString error;
        QVERIFY2(remote::restrictToCurrentUser(directory.path(), true, &error), qPrintable(error));

        PACL acl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const QString native = QDir::toNativeSeparators(directory.path());
        QCOMPARE(::GetNamedSecurityInfoW(reinterpret_cast<LPCWSTR>(native.utf16()), SE_FILE_OBJECT,
                                         DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr,
                                         &descriptor),
                 DWORD(ERROR_SUCCESS));
        const auto free = qScopeGuard([descriptor] { ::LocalFree(descriptor); });
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        QVERIFY(::GetSecurityDescriptorControl(descriptor, &control, &revision));
        QVERIFY2(control & SE_DACL_PROTECTED, "inheritance from the parent must be blocked");
        QCOMPARE(int(acl->AceCount), 1);
        void* entry = nullptr;
        QVERIFY(::GetAce(acl, 0, &entry));
        const auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(entry);
        QCOMPARE(int(allowed->Header.AceType), int(ACCESS_ALLOWED_ACE_TYPE));

        HANDLE token = nullptr;
        QVERIFY(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token));
        const auto closeToken = qScopeGuard([token] { ::CloseHandle(token); });
        DWORD needed = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        QByteArray user(static_cast<qsizetype>(needed), '\0');
        QVERIFY(::GetTokenInformation(token, TokenUser, user.data(), needed, &needed));
        QVERIFY(::EqualSid(reinterpret_cast<PSID>(const_cast<DWORD*>(&allowed->SidStart)),
                           reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid));

        // New files inherit the entry.
        QFile child(directory.filePath(QStringLiteral("child.txt")));
        QVERIFY(child.open(QIODevice::WriteOnly));
        child.close();
        PACL childAcl = nullptr;
        PSECURITY_DESCRIPTOR childDescriptor = nullptr;
        const QString childNative = QDir::toNativeSeparators(child.fileName());
        QCOMPARE(::GetNamedSecurityInfoW(reinterpret_cast<LPCWSTR>(childNative.utf16()),
                                         SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                         nullptr, &childAcl, nullptr, &childDescriptor),
                 DWORD(ERROR_SUCCESS));
        const auto freeChild = qScopeGuard([childDescriptor] { ::LocalFree(childDescriptor); });
        QCOMPARE(int(childAcl->AceCount), 1);
#else
        QSKIP("Windows access lists only.");
#endif
    }

    void killingAProviderEndsItsHelpersToo() {
#if defined(Q_OS_WIN)
        QProcess process;
        daemon::attachProcessTree(&process);
        // cmd starts ping as a child; ping outlives a plain kill of cmd.
        process.start(QStringLiteral("cmd.exe"),
                      {QStringLiteral("/c"), QStringLiteral("ping -n 60 127.0.0.1 > nul")});
        QVERIFY(process.waitForStarted(5000));
        const DWORD parent = static_cast<DWORD>(process.processId());

        HANDLE helper = nullptr;
        for (int attempt = 0; attempt < 50 && helper == nullptr; ++attempt) {
            QTest::qWait(100);
            HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL more = ::Process32FirstW(snapshot, &entry); more && helper == nullptr;
                 more = ::Process32NextW(snapshot, &entry))
                if (entry.th32ParentProcessID == parent)
                    helper = ::OpenProcess(SYNCHRONIZE, FALSE, entry.th32ProcessID);
            ::CloseHandle(snapshot);
        }
        QVERIFY2(helper != nullptr, "the helper process never appeared");
        const auto close = qScopeGuard([helper] { ::CloseHandle(helper); });
        QCOMPARE(::WaitForSingleObject(helper, 0), DWORD(WAIT_TIMEOUT));

        daemon::killProcessTree(&process);
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(::WaitForSingleObject(helper, 5000), DWORD(WAIT_OBJECT_0));
#else
        QSKIP("Job Objects are Windows-only.");
#endif
    }
};

QTEST_GUILESS_MAIN(WindowsPlatformTest)
#include "WindowsPlatformTest.moc"
