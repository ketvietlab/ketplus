#include "MemoryCredentials.h"
#include "cloud/CredentialStore.h"
#include <QStringList>
#include <utility>
namespace r03 {
QHash<QString, QByteArray> credentials;
bool failWrites = false;
bool failRemovals = false;
QString failWritesTo;
QString failRemovalsOf;
int rewrites = 0;
void resetCredentials() {
    credentials.clear();
    failWrites = false;
    failRemovals = false;
    failWritesTo.clear();
    failRemovalsOf.clear();
    rewrites = 0;
}
} // namespace r03
namespace ketplus::daemon {
CredentialStore::CredentialStore(QString service) : service_(std::move(service)) {}
QByteArray CredentialStore::read(const QString& account, QString*) const {
    return r03::credentials.value(service_ + '/' + account);
}
bool CredentialStore::write(const QString& account, const QByteArray& value, QString* error) const {
    if (r03::failWrites || (!r03::failWritesTo.isEmpty() && account == r03::failWritesTo)) {
        if (error)
            *error = "synthetic credential write failure";
        return false;
    }
    r03::credentials.insert(service_ + '/' + account, value);
    ++r03::rewrites;
    return true;
}
bool CredentialStore::remove(const QString& account, QString* error) const {
    return removeIf([&account](const QString& candidate) { return candidate == account; }, error);
}
bool CredentialStore::removeIf(const std::function<bool(const QString& account)>& match,
                               QString* error) const {
    // Fails as a removal of the named account did, whether or not it is stored.
    if (r03::failRemovals || (!r03::failRemovalsOf.isEmpty() && match(r03::failRemovalsOf))) {
        if (error)
            *error = "synthetic credential removal failure";
        return false;
    }
    return update(
        [&match](QHash<QString, QByteArray>& accounts) {
            accounts.removeIf([&match](const auto& entry) { return match(entry.key()); });
        },
        error);
}
bool CredentialStore::update(
    const std::function<void(QHash<QString, QByteArray>& accounts)>& change, QString* error) const {
    const QString prefix = service_ + '/';
    QHash<QString, QByteArray> accounts;
    for (auto entry = r03::credentials.cbegin(); entry != r03::credentials.cend(); ++entry) {
        if (entry.key().startsWith(prefix))
            accounts.insert(entry.key().mid(prefix.size()), entry.value());
    }
    const QHash<QString, QByteArray> before = accounts;
    change(accounts);
    if (accounts == before)
        return true;
    // Fails as a write of each written account did.
    for (auto entry = accounts.cbegin(); entry != accounts.cend(); ++entry) {
        const bool written =
            !before.contains(entry.key()) || before.value(entry.key()) != entry.value();
        if (written && (r03::failWrites || entry.key() == r03::failWritesTo)) {
            if (error)
                *error = "synthetic credential write failure";
            return false;
        }
    }
    for (auto entry = before.cbegin(); entry != before.cend(); ++entry) {
        if (!accounts.contains(entry.key()))
            r03::credentials.remove(prefix + entry.key());
    }
    for (auto entry = accounts.cbegin(); entry != accounts.cend(); ++entry)
        r03::credentials.insert(prefix + entry.key(), entry.value());
    ++r03::rewrites;
    return true;
}
} // namespace ketplus::daemon
