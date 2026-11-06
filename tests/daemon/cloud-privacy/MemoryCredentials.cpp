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
    return rewrite(match, {}, error);
}
bool CredentialStore::rewrite(const std::function<bool(const QString& account)>& match,
                              const QHash<QString, QByteArray>& writes, QString* error) const {
    if ((r03::failWrites && !writes.isEmpty()) ||
        (!r03::failWritesTo.isEmpty() && writes.contains(r03::failWritesTo))) {
        if (error)
            *error = "synthetic credential write failure";
        return false;
    }
    const QString prefix = service_ + '/';
    QStringList matching;
    for (auto entry = r03::credentials.cbegin(); entry != r03::credentials.cend(); ++entry) {
        if (entry.key().startsWith(prefix) && match(entry.key().mid(prefix.size())))
            matching.append(entry.key());
    }
    // Fails as a removal of the named account did, whether or not it is stored.
    if (r03::failRemovals || (!r03::failRemovalsOf.isEmpty() && match(r03::failRemovalsOf))) {
        if (error)
            *error = "synthetic credential removal failure";
        return false;
    }
    if (matching.isEmpty() && writes.isEmpty())
        return true;
    for (const QString& key : matching)
        r03::credentials.remove(key);
    for (auto write = writes.cbegin(); write != writes.cend(); ++write)
        r03::credentials.insert(prefix + write.key(), write.value());
    ++r03::rewrites;
    return true;
}
} // namespace ketplus::daemon
