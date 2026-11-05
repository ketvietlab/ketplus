#include "MemoryCredentials.h"
#include "cloud/CredentialStore.h"
#include <utility>
namespace r03 {
QHash<QString, QByteArray> credentials;
bool failWrites = false;
bool failRemovals = false;
void resetCredentials() {
    credentials.clear();
    failWrites = false;
    failRemovals = false;
}
} // namespace r03
namespace ketplus::daemon {
CredentialStore::CredentialStore(QString service) : service_(std::move(service)) {}
QByteArray CredentialStore::read(const QString& account, QString*) const {
    return r03::credentials.value(service_ + '/' + account);
}
bool CredentialStore::write(const QString& account, const QByteArray& value, QString* error) const {
    if (r03::failWrites) {
        if (error)
            *error = "synthetic credential write failure";
        return false;
    }
    r03::credentials.insert(service_ + '/' + account, value);
    return true;
}
bool CredentialStore::remove(const QString& account, QString* error) const {
    if (r03::failRemovals) {
        if (error)
            *error = "synthetic credential removal failure";
        return false;
    }
    r03::credentials.remove(service_ + '/' + account);
    return true;
}
} // namespace ketplus::daemon
