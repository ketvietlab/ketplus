#include "agent/ConversationWorktreeStore.h"

#include "LinkedWorktreeRemoval.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QUuid>
#include <QtDebug>

namespace ketplus {
namespace {
class Connection final {
  public:
    explicit Connection(const QString& path, bool write) {
        if (!write && !QFileInfo::exists(path))
            return;
        if (write && !QDir().mkpath(QFileInfo(path).absolutePath()))
            return;
        name_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
        db_.setDatabaseName(path);
        db_.setConnectOptions(write ? "QSQLITE_BUSY_TIMEOUT=100"
                                    : "QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=0");
        if (!db_.open())
            return;
        if (write) {
            QSqlQuery query(db_);
            query.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
            ready_ = query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS conversation_worktrees ("
                                               "conversation_id TEXT NOT NULL, "
                                               "path TEXT NOT NULL, "
                                               "role TEXT NOT NULL, "
                                               "hidden INTEGER NOT NULL DEFAULT 0, "
                                               "created_at INTEGER NOT NULL, "
                                               "git_identity TEXT NOT NULL DEFAULT '', "
                                               "PRIMARY KEY (conversation_id, path))"));
            if (ready_ && !hasGitIdentity())
                ready_ = query.exec(
                             QStringLiteral("ALTER TABLE conversation_worktrees "
                                            "ADD COLUMN git_identity TEXT NOT NULL DEFAULT ''")) ||
                         hasGitIdentity();
        } else
            ready_ = true;
    }
    ~Connection() {
        db_ = QSqlDatabase();
        if (!name_.isEmpty())
            QSqlDatabase::removeDatabase(name_);
    }
    [[nodiscard]] bool hasGitIdentity() const {
        return db_.record(QStringLiteral("conversation_worktrees"))
            .contains(QStringLiteral("git_identity"));
    }

    QSqlDatabase db_;
    bool ready_{false};

  private:
    QString name_;
};

QString cleanPath(const QString& path) {
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty())
        return QDir::cleanPath(canonical);
    // A deleted worktree has no canonical path. Resolve its nearest existing folder, so a
    // path under a symlinked parent (/tmp -> /private/tmp) still matches the stored row.
    QString existing = QDir::cleanPath(info.absoluteFilePath());
    QStringList missing;
    while (!QFileInfo::exists(existing)) {
        const QString parent = QFileInfo(existing).absolutePath();
        if (parent == existing)
            return QDir::cleanPath(info.absoluteFilePath());
        missing.prepend(QFileInfo(existing).fileName());
        existing = parent;
    }
    QString resolved = QFileInfo(existing).canonicalFilePath();
    for (const QString& part : missing)
        resolved = QDir(resolved).filePath(part);
    return QDir::cleanPath(resolved);
}

void execOrWarn(QSqlQuery& query, const char* action) {
    if (!query.exec())
        qWarning().noquote() << "KetPlus: conversation worktree" << action
                             << "failed:" << query.lastError().text();
}

} // namespace

ConversationWorktreeStore::ConversationWorktreeStore(QString databasePath)
    : databasePath_(
          databasePath.isEmpty()
              ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                    .filePath(QStringLiteral("conversation-worktrees.sqlite"))
              : std::move(databasePath)) {}

void ConversationWorktreeStore::save(const QString& conversationId, const QString& path,
                                     const QString& role) {
    const QString id = conversationId.trimmed();
    const QString worktree = cleanPath(path);
    if (id.isEmpty() || worktree.isEmpty())
        return;
    cache_.remove(id);
    Connection connection(databasePath_, true);
    if (!connection.ready_)
        return;
    QSqlQuery query(connection.db_);
    query.prepare(QStringLiteral(
        "INSERT INTO conversation_worktrees(conversation_id, path, role, hidden, created_at, "
        "git_identity) VALUES(?, ?, ?, 0, ?, ?) "
        "ON CONFLICT(conversation_id, path) DO UPDATE SET role=excluded.role, hidden=0, "
        "git_identity=excluded.git_identity"));
    query.addBindValue(id);
    query.addBindValue(worktree);
    query.addBindValue(role.isEmpty() ? QString::fromLatin1(kRoleAgent) : role);
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    const auto identity = git::linkedWorktreeIdentity(worktree);
    query.addBindValue(identity.isEmpty() ? QStringLiteral("") : identity);
    execOrWarn(query, "save");
}

void ConversationWorktreeStore::hide(const QString& conversationId, const QString& path) {
    cache_.remove(conversationId);
    Connection connection(databasePath_, true);
    if (!connection.ready_)
        return;
    QSqlQuery query(connection.db_);
    query.prepare(QStringLiteral(
        "UPDATE conversation_worktrees SET hidden=1 WHERE conversation_id=? AND path=?"));
    query.addBindValue(conversationId);
    query.addBindValue(cleanPath(path));
    execOrWarn(query, "hide");
}

void ConversationWorktreeStore::removeConversation(const QString& conversationId) {
    cache_.remove(conversationId);
    Connection connection(databasePath_, true);
    if (!connection.ready_)
        return;
    QSqlQuery query(connection.db_);
    query.prepare(QStringLiteral("DELETE FROM conversation_worktrees WHERE conversation_id=?"));
    query.addBindValue(conversationId);
    execOrWarn(query, "removal");
}

QVector<ConversationWorktreeRecord>
ConversationWorktreeStore::list(const QString& conversationId) const {
    const auto cached = cache_.constFind(conversationId);
    QVector<ConversationWorktreeRecord> result;
    if (cached != cache_.constEnd()) {
        result = *cached;
    } else {
        Connection connection(databasePath_, false);
        if (!connection.ready_)
            return result;
        QSqlQuery query(connection.db_);
        const auto identity =
            connection.hasGitIdentity() ? QStringLiteral("git_identity") : QStringLiteral("''");
        query.prepare(QStringLiteral("SELECT conversation_id, path, role, hidden, %1 FROM "
                                     "conversation_worktrees WHERE conversation_id=? "
                                     "ORDER BY created_at ASC")
                          .arg(identity));
        query.addBindValue(conversationId);
        if (!query.exec())
            return result;
        while (query.next()) {
            result.append({query.value(0).toString(), query.value(1).toString(),
                           query.value(2).toString(), query.value(3).toBool(),
                           query.value(4).toString()});
        }
        cache_.insert(conversationId, result);
    }
    // Keep a missing path in the history. If another checkout takes its place,
    // the old row must never display or offer to delete the replacement.
    result.removeIf([](const auto& record) {
        return !record.gitIdentity.isEmpty() && QFileInfo::exists(record.path) &&
               git::linkedWorktreeIdentity(record.path) != record.gitIdentity;
    });
    return result;
}

QStringList ConversationWorktreeStore::visiblePaths(const QString& conversationId) const {
    QStringList paths;
    for (const auto& record : list(conversationId)) {
        if (!record.hidden)
            paths.append(record.path);
    }
    return paths;
}

QStringList ConversationWorktreeStore::allPaths(const QString& conversationId) const {
    QStringList paths;
    for (const auto& record : list(conversationId))
        paths.append(record.path);
    return paths;
}

QStringList ConversationWorktreeStore::removablePaths(const QString& conversationId) const {
    QStringList paths;
    for (const auto& record : list(conversationId)) {
        if (!record.gitIdentity.isEmpty() &&
            git::linkedWorktreeIdentity(record.path) == record.gitIdentity)
            paths.append(record.path);
    }
    return paths;
}

bool ConversationWorktreeStore::containsPath(const QString& path,
                                             const QString& exceptConversationId) const {
    const QString worktree = cleanPath(path);
    if (worktree.isEmpty())
        return false;
    Connection connection(databasePath_, false);
    if (!connection.ready_)
        return false;
    QSqlQuery query(connection.db_);
    const auto identityColumn =
        connection.hasGitIdentity() ? QStringLiteral("git_identity") : QStringLiteral("''");
    query.prepare(QStringLiteral("SELECT conversation_id, %1 FROM conversation_worktrees "
                                 "WHERE path=?")
                      .arg(identityColumn));
    query.addBindValue(worktree);
    if (!query.exec())
        return false;
    const bool exists = QFileInfo::exists(worktree);
    const QString identity = exists ? git::linkedWorktreeIdentity(worktree) : QString{};
    while (query.next()) {
        const QString recorded = query.value(1).toString();
        if (query.value(0).toString() != exceptConversationId &&
            (!exists || recorded.isEmpty() || recorded == identity))
            return true;
    }
    return false;
}

QStringList ConversationWorktreeStore::createdDuringTurn(const QStringList& before,
                                                         const QStringList& after,
                                                         const QString& mainCheckout) {
    QSet<QString> known;
    for (const QString& path : before)
        known.insert(cleanPath(path));
    const QString main = cleanPath(mainCheckout);
    QStringList created;
    for (const QString& path : after) {
        const QString worktree = cleanPath(path);
        if (worktree.isEmpty() || worktree == main || known.contains(worktree))
            continue;
        created.append(worktree);
        known.insert(worktree);
    }
    return created;
}

} // namespace ketplus
