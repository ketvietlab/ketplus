#include "cloud/SessionCredentials.h"

#include "cloud/CredentialStore.h"

#include <QCryptographicHash>

namespace ketplus::daemon {
namespace {

const QString keyPrefix = QStringLiteral("session-key-");
const QString wrapPrefix = QStringLiteral("wrapped-");
const QString cursorPrefix = QStringLiteral("uploaded-cursor-");
const QString workspaceAccount = QStringLiteral("session-keys-workspace");
constexpr qsizetype digestLength = 64;

bool isSessionAccount(const QString& account) {
    return account.startsWith(keyPrefix) || account.startsWith(wrapPrefix) ||
           account.startsWith(cursorPrefix);
}

// Whether the account names a live session. A wrap or cursor names the session by its id, which
// may itself contain '-', so every prefix ending before a '-' is tried: an account is removed
// only when none of them is live.
bool namesLiveSession(const QString& account, const QSet<QString>& liveIds,
                      const QSet<QString>& liveDigests) {
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
        liveDigests.insert(QString::fromLatin1(
            QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex()));
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
    return store_->rewrite(isSessionAccount, {{workspaceAccount, workspaceId.toUtf8()}}, error);
}

} // namespace ketplus::daemon
