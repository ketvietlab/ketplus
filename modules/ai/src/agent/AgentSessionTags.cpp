#include "agent/AgentSessionManager.h"

#include "agent/AgentSessionState.h"
#include "agent/CodexAppServer.h"
#include "agent/ConversationTags.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QSet>

#include <array>
#include <utility>

namespace ketplus {
namespace conversation_tags {
namespace {

struct Tag {
    QLatin1StringView label;
    QLatin1StringView color;
};

constexpr QLatin1StringView neutral{"#5b6b7f"};
constexpr std::array<Tag, 7> tags{{{QLatin1StringView{"UI"}, QLatin1StringView{"#4f6bed"}},
                                   {QLatin1StringView{"Performance"}, QLatin1StringView{"#c2780a"}},
                                   {QLatin1StringView{"Review"}, QLatin1StringView{"#7c5cd6"}},
                                   {QLatin1StringView{"Atlas"}, QLatin1StringView{"#3f8f7a"}},
                                   {QLatin1StringView{"Account"}, QLatin1StringView{"#2f8f4e"}},
                                   {QLatin1StringView{"Bug"}, QLatin1StringView{"#d1453b"}},
                                   {QLatin1StringView{"Docs"}, neutral}}};

} // namespace

QVariantList catalog() {
    QVariantList result;
    for (const Tag& tag : tags)
        result.append(QVariantMap{{QStringLiteral("label"), QString(tag.label)},
                                  {QStringLiteral("color"), QString(tag.color)}});
    return result;
}

QString color(const QString& label) {
    for (const Tag& tag : tags)
        if (label.compare(tag.label, Qt::CaseInsensitive) == 0)
            return QString(tag.color);
    return QString(neutral);
}

} // namespace conversation_tags

QVariantList AgentSessionManager::listedTags(const SessionPtr& session) const {
    QVariantList result;
    for (const QString& label : session->tags) {
        QString color = conversation_tags::color(label);
        for (const QVariant& value : tagCatalog_) {
            const QVariantMap tag = value.toMap();
            if (tag.value(QStringLiteral("label")).toString().compare(label, Qt::CaseInsensitive) ==
                0) {
                color = tag.value(QStringLiteral("color")).toString();
                break;
            }
        }
        result.append(
            QVariantMap{{QStringLiteral("label"), label}, {QStringLiteral("color"), color}});
    }
    return result;
}

// #429: the daemon keeps the catalog; until it answers (or when it is too old to keep one)
// the built-in labels are offered. Another window's change arrives as `tags.changed`.
void AgentSessionManager::installTagCatalog() {
    tagCatalog_ = conversation_tags::catalog();
    connect(server_, &CodexAppServer::readyChanged, this, [this](const bool ready) {
        if (ready)
            loadTagCatalog();
    });
    connect(server_, &CodexAppServer::tagsChanged, this, &AgentSessionManager::applyTagChange);
    if (server_->isReady())
        loadTagCatalog();
}

void AgentSessionManager::loadTagCatalog() {
    if (!server_->hasTagCatalogSupport())
        return;
    server_->readTagCatalog([this, guard = QPointer<AgentSessionManager>(this)](
                                const QJsonObject& result, const QString& error) {
        if (guard && error.isEmpty())
            setTagCatalogFromDaemon(result.value(QStringLiteral("tags")).toArray());
    });
}

void AgentSessionManager::setTagCatalogFromDaemon(const QJsonArray& catalog) {
    QVariantList next;
    for (const auto& value : catalog) {
        const QJsonObject tag = value.toObject();
        next.append(QVariantMap{{QStringLiteral("label"), tag.value(QStringLiteral("label"))},
                                {QStringLiteral("color"), tag.value(QStringLiteral("color"))}});
    }
    if (next == tagCatalog_)
        return;
    tagCatalog_ = next;
    emit tagCatalogChanged(tagCatalog_);
    // Colours may have changed on every listed tag.
    for (auto it = conversationsByPath_.cbegin(); it != conversationsByPath_.cend(); ++it)
        emit conversationsChanged(it.key(), conversations(it.key()));
}

void AgentSessionManager::applyTagChange(const QJsonObject& change) {
    if (change.contains(QStringLiteral("catalog")))
        setTagCatalogFromDaemon(change.value(QStringLiteral("catalog")).toArray());
    QSet<QString> changedPaths;
    for (const auto& value : change.value(QStringLiteral("sessions")).toArray()) {
        const QJsonObject entry = value.toObject();
        const SessionPtr session =
            sessionsByThread_.value(entry.value(QStringLiteral("session_id")).toString());
        if (!session || session->tagSyncPending)
            continue; // Its own request's answer sets it.
        const QStringList tags = entry.value(QStringLiteral("tags")).toVariant().toStringList();
        if (session->tags == tags)
            continue;
        session->tags = tags;
        changedPaths.insert(session->path);
    }
    for (const QString& path : std::as_const(changedPaths))
        emit conversationsChanged(path, conversations(path));
}

void AgentSessionManager::saveTagCatalog(const QVariantList& tags, const QVariantMap& renames) {
    QJsonArray catalog;
    for (const QVariant& value : tags) {
        const QVariantMap tag = value.toMap();
        catalog.append(
            QJsonObject{{QStringLiteral("label"), tag.value(QStringLiteral("label")).toString()},
                        {QStringLiteral("color"), tag.value(QStringLiteral("color")).toString()}});
    }
    server_->setTagCatalog(
        catalog, QJsonObject::fromVariantMap(renames),
        [this, guard = QPointer<AgentSessionManager>(this)](const QJsonObject& result,
                                                            const QString& error) {
            if (!guard)
                return;
            if (!error.isEmpty()) {
                emit errorOccurred(QStringLiteral("The tags could not be saved: %1").arg(error));
                return;
            }
            applyTagChange(
                {{QStringLiteral("catalog"), result.value(QStringLiteral("tags"))},
                 {QStringLiteral("sessions"), result.value(QStringLiteral("sessions"))}});
        });
}

void AgentSessionManager::sendStartTags(const SessionPtr& session) {
    if (!std::exchange(session->tagsAwaitStart, false) || !server_->hasTagSupport())
        return;
    session->tagsWanted = session->tags;
    syncTags(session);
}

// The daemon owns the labels: the list changes here only once it has stored the new one, so
// every client and the next start agree. Changes made while a request is on its way add up in
// tagsWanted and go out as one list afterwards, so a second change cannot undo the first.
void AgentSessionManager::setConversationTag(const QString& worktreePath,
                                             const QString& conversationId, const QString& label,
                                             const bool tagged) {
    const QString name = label.trimmed();
    const QString path = normalizedPath(worktreePath);
    for (const SessionPtr& session : conversationsByPath_.value(path)) {
        if (session->id != conversationId)
            continue;
        QStringList tags = session->tagSyncPending ? session->tagsWanted : session->tags;
        if (name.isEmpty() || tags.contains(name, Qt::CaseInsensitive) == tagged)
            return;
        if (!server_->hasTagSupport()) {
            emit errorOccurred(QStringLiteral("This daemon does not store conversation tags."));
            return;
        }
        if (tagged)
            tags.append(name);
        else
            tags.removeIf([&name](const QString& existing) {
                return existing.compare(name, Qt::CaseInsensitive) == 0;
            });
        if (session->threadId.isEmpty()) {
            // Not started yet: kept here and sent with the first turn (sendStartTags).
            session->tags = tags;
            session->tagsAwaitStart = true;
            emit conversationsChanged(session->path, conversations(session->path));
            return;
        }
        session->tagsWanted = tags;
        syncTags(session);
        return;
    }
}

// #125: the daemon clears it first, so another client or the next start does not show it again.
void AgentSessionManager::markConversationDone(const QString& worktreePath,
                                               const QString& conversationId) {
    for (const SessionPtr& session : conversationsByPath_.value(normalizedPath(worktreePath))) {
        if (session->id != conversationId || !session->awaitingReply)
            continue;
        server_->markDaemonSessionDone(
            session->threadId, [this, weak = std::weak_ptr<Session>(session)](
                                   const QJsonObject&, const QString& error) {
                const SessionPtr current = weak.lock();
                if (!current)
                    return;
                if (!error.isEmpty()) {
                    emit errorOccurred(QStringLiteral("Could not mark it as done: %1").arg(error));
                    return;
                }
                current->awaitingReply = false;
                emit conversationsChanged(current->path, conversations(current->path));
            });
        return;
    }
}

void AgentSessionManager::syncTags(const SessionPtr& session) {
    if (session->tagSyncPending)
        return;
    session->tagSyncPending = true;
    const QStringList sent = session->tagsWanted;
    server_->setDaemonSessionTags(
        session->threadId, sent,
        [this, sent, weak = std::weak_ptr<Session>(session)](const QJsonObject& result,
                                                             const QString& error) {
            const SessionPtr current = weak.lock();
            if (!current)
                return;
            current->tagSyncPending = false;
            if (!error.isEmpty()) {
                // Later changes were built on the list that failed; drop them with it.
                current->tagsWanted.clear();
                emit errorOccurred(QStringLiteral("The tags could not be saved: %1").arg(error));
                return;
            }
            const QJsonValue stored = result.value(QStringLiteral("tags"));
            current->tags = stored.isArray() ? stored.toVariant().toStringList() : sent;
            emit conversationsChanged(current->path, conversations(current->path));
            if (current->tagsWanted != sent)
                syncTags(current);
            else
                current->tagsWanted.clear();
        });
}

} // namespace ketplus
