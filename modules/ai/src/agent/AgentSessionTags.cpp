#include "agent/AgentSessionManager.h"

#include "agent/AgentSessionState.h"
#include "agent/CodexAppServer.h"
#include "agent/ConversationTags.h"

#include <QJsonArray>
#include <QJsonObject>

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
    for (const QString& label : session->tags)
        result.append(QVariantMap{{QStringLiteral("label"), label},
                                  {QStringLiteral("color"), conversation_tags::color(label)}});
    return result;
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
        if (session->threadId.isEmpty() || !server_->hasTagSupport()) {
            emit errorOccurred(
                QStringLiteral("Tags can be set once the conversation has started."));
            return;
        }
        if (tagged)
            tags.append(name);
        else
            tags.removeIf([&name](const QString& existing) {
                return existing.compare(name, Qt::CaseInsensitive) == 0;
            });
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
