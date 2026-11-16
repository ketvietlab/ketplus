#pragma once
#include <QByteArray>
#include <QString>
#include <memory>

namespace ketplus::daemon {
// One service record is replaced atomically by the OS store. Missing is distinct from failure.
class CredentialBackend {
  public:
    enum class Result { Found, Missing, Error };
    virtual ~CredentialBackend() = default;
    virtual Result read(const QString& service, QByteArray* value, QString* error) = 0;
    virtual bool write(const QString& service, const QByteArray& value, QString* error) = 0;
};
std::shared_ptr<CredentialBackend> nativeCredentialBackend();
} // namespace ketplus::daemon
