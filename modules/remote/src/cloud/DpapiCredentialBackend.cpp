#include "DaemonPaths.h"
#include "OwnerOnlyAccess.h"
#include "cloud/CredentialBackend.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStringList>
#include <qt_windows.h>

#include <dpapi.h>

namespace ketplus::daemon {
namespace {
// One file per service, sealed with DPAPI for the current user: another account, or the same
// disk read offline, cannot open it. The service name is already validated by CredentialStore
// (letters, digits, '_', '.', '-'), so it cannot escape the directory.
class DpapiCredentialBackend final : public CredentialBackend {
  public:
    Result read(const QString& service, QByteArray* value, QString* error) override {
        value->clear();
        QFile file(path(service));
        if (!file.exists())
            return Result::Missing;
        if (!file.open(QIODevice::ReadOnly))
            return fail(error, QStringLiteral("Unable to read the protected credential record."));
        QByteArray sealed = file.readAll();
        DATA_BLOB input{static_cast<DWORD>(sealed.size()), reinterpret_cast<BYTE*>(sealed.data())};
        DATA_BLOB output{};
        if (!::CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                                  CRYPTPROTECT_UI_FORBIDDEN, &output))
            return fail(error, QStringLiteral("The protected credential record cannot be opened "
                                              "by this Windows account."));
        *value = QByteArray(reinterpret_cast<const char*>(output.pbData),
                            static_cast<qsizetype>(output.cbData));
        ::SecureZeroMemory(output.pbData, output.cbData);
        ::LocalFree(output.pbData);
        return Result::Found;
    }

    bool write(const QString& service, const QByteArray& value, QString* error) override {
        QByteArray plain = value;
        DATA_BLOB input{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(plain.data())};
        DATA_BLOB output{};
        const bool sealed = ::CryptProtectData(&input, L"KetPlus credential", nullptr, nullptr,
                                               nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
        ::SecureZeroMemory(plain.data(), static_cast<SIZE_T>(plain.size()));
        if (!sealed) {
            fail(error, QStringLiteral("Unable to protect the credential record."));
            return false;
        }
        const auto release = qScopeGuard([&] { ::LocalFree(output.pbData); });
        const QString target = path(service);
        const QString directory = QFileInfo(target).absolutePath();
        if (!QDir().mkpath(directory) || !remote::restrictToCurrentUser(directory, true, error)) {
            fail(error, QStringLiteral("Unable to prepare the credential directory."));
            return false;
        }
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(reinterpret_cast<const char*>(output.pbData),
                       static_cast<qint64>(output.cbData)) != static_cast<qint64>(output.cbData) ||
            !file.commit()) {
            fail(error, QStringLiteral("Unable to store the protected credential record."));
            return false;
        }
        return true;
    }

  private:
    static QString path(const QString& service) {
        return QDir(remote::DaemonPaths::dataDirectory())
            .filePath(QStringLiteral("daemon/protected/%1.dpapi").arg(service));
    }
    static Result fail(QString* error, const QString& message) {
        if (error)
            *error = message;
        return Result::Error;
    }
};
} // namespace

std::shared_ptr<CredentialBackend> platformCredentialBackend() {
    return std::make_shared<DpapiCredentialBackend>();
}
} // namespace ketplus::daemon
