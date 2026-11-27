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
    // Whether the store would serve the record now, without decrypting it. A cached record is
    // used only while this holds, so a locked store stays locked for a running process too.
    virtual bool accessible(const QString& service, QString* error) {
        Q_UNUSED(service);
        Q_UNUSED(error);
        return true;
    }
};
std::shared_ptr<CredentialBackend> nativeCredentialBackend();
} // namespace ketplus::daemon
