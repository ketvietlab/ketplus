#pragma once
#include "cloud/CredentialBackend.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusObjectPath>
namespace ketplus::daemon {
struct SecretServiceValue {
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};
QDBusArgument& operator<<(QDBusArgument&, const SecretServiceValue&);
const QDBusArgument& operator>>(const QDBusArgument&, SecretServiceValue&);
std::shared_ptr<CredentialBackend>
secretServiceCredentialBackend(const QDBusConnection& connection);
} // namespace ketplus::daemon
Q_DECLARE_METATYPE(ketplus::daemon::SecretServiceValue)
