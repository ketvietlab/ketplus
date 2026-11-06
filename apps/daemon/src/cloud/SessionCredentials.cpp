#include "cloud/SessionCredentials.h"

#include "cloud/CredentialStore.h"

#include <QCryptographicHash>

namespace ketplus::daemon {
namespace {

const QString keyPrefix = QStringLiteral("session-key-");
const QString wrapPrefix = QStringLiteral("wrapped-");
const QString cursorPrefix = QStringLiteral("uploaded-cursor-");
const QString workspaceAccount = QStringLiteral("session-keys-workspace");
const QString setAsidePrefix = QStringLiteral("workspace-keys-");
constexpr qsizetype digestLength = 64;

QString digest(const QString& value) {
    return QString::fromLatin1(
        QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString setAside(const QString& workspaceId) { return setAsidePrefix + digest(workspaceId) + u'-'; }

// The account a set-aside key or wrap was stored under while its workspace was in use.
QString inUse(const QString& account) {
    return account.startsWith(setAsidePrefix)
               ? account.mid(setAsidePrefix.size() + digestLength + 1)
               : account;
}

bool isSessionAccount(const QString& account) {
    const QString name = inUse(account);
    return name.startsWith(keyPrefix) || name.startsWith(wrapPrefix) ||
           name.startsWith(cursorPrefix);
}

// Whether the account names a live session. A wrap or cursor names the session by its id, which
// may itself contain '-', so every prefix ending before a '-' is tried: an account is removed
// only when none of them is live.
bool namesLiveSession(const QString& stored, const QSet<QString>& liveIds,
                      const QSet<QString>& liveDigests) {
    const QString account = inUse(stored);
    if (account.startsWith(keyPrefix))
        return liveDigests.contains(account.mid(keyPrefix.size(), digestLength));
    const QString rest =
        account.mid(account.startsWith(wrapPrefix) ? wrapPrefix.size() : cursorPrefix.size());
    for (qsizetype dash = rest.indexOf(u'-'); dash > 0; dash = rest.indexOf(u'-', dash + 1)) {
        if (liveIds.contains(rest.left(dash)))
            return true;
    }
    return false;
}

} // namespace

SessionCredentials::SessionCredentials(CredentialStore* store) : store_(store) {}

bool SessionCredentials::isUploadCursor(const QString& account) {
    return account.startsWith(cursorPrefix);
}

bool SessionCredentials::clearUploadCursors(QString* error) const {
    return store_->removeIf(isUploadCursor, error);
}

bool SessionCredentials::pruneDeleted(const QSet<QString>& liveSessionIds, QString* error) const {
    QSet<QString> liveDigests;
    for (const QString& id : liveSessionIds)
        liveDigests.insert(digest(id));
    return store_->removeIf(
        [&](const QString& account) {
            return isSessionAccount(account) &&
                   !namesLiveSession(account, liveSessionIds, liveDigests);
        },
        error);
}

bool SessionCredentials::adoptWorkspace(const QString& workspaceId, QString* error) const {
    QString readError;
    const QString recorded = QString::fromUtf8(store_->read(workspaceAccount, &readError));
    if (!readError.isEmpty()) {
        if (error != nullptr)
            *error = readError;
        return false;
    }
    if (recorded == workspaceId)
        return true;
    if (recorded.isEmpty())
        return store_->write(workspaceAccount, workspaceId.toUtf8(), error);
    const QString leaving = setAside(recorded);
    const QString returning = setAside(workspaceId);
    return store_->update(
        [&](QHash<QString, QByteArray>& accounts) {
            QHash<QString, QByteArray> moved;
            for (auto entry = accounts.begin(); entry != accounts.end();) {
                const QString& account = entry.key();
                const bool used = account.startsWith(keyPrefix) || account.startsWith(wrapPrefix);
                const bool returned = account.startsWith(returning);
                if (!used && !returned && !account.startsWith(cursorPrefix)) {
                    ++entry;
                    continue;
                }
                if (used)
                    moved.insert(leaving + account, entry.value());
                else if (returned)
                    moved.insert(account.mid(returning.size()), entry.value());
                entry = accounts.erase(entry);
            }
            accounts.insert(moved);
            accounts.insert(workspaceAccount, workspaceId.toUtf8());
        },
        error);
}

} // namespace ketplus::daemon
