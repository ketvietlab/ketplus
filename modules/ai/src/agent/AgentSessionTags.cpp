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
// every client and the next start agree.
void AgentSessionManager::setConversationTag(const QString& worktreePath,
                                             const QString& conversationId, const QString& label,
                                             const bool tagged) {
    const QString name = label.trimmed();
    const QString path = normalizedPath(worktreePath);
    for (const SessionPtr& session : conversationsByPath_.value(path)) {
        if (session->id != conversationId)
            continue;
        if (name.isEmpty() || session->tags.contains(name, Qt::CaseInsensitive) == tagged)
            return;
        if (session->threadId.isEmpty() || !server_->hasTagSupport()) {
            emit errorOccurred(
                QStringLiteral("Tags can be set once the conversation has started."));
            return;
        }
        QStringList tags = session->tags;
        if (tagged)
            tags.append(name);
        else
            tags.removeIf([&name](const QString& existing) {
                return existing.compare(name, Qt::CaseInsensitive) == 0;
            });
        server_->setDaemonSessionTags(
            session->threadId, tags,
            [this, tags, weak = std::weak_ptr<Session>(session)](const QJsonObject& result,
                                                                 const QString& error) {
                const SessionPtr current = weak.lock();
                if (!current)
                    return;
                if (!error.isEmpty()) {
                    emit errorOccurred(
                        QStringLiteral("The tags could not be saved: %1").arg(error));
                    return;
                }
                const QJsonValue stored = result.value(QStringLiteral("tags"));
                current->tags = stored.isArray() ? stored.toVariant().toStringList() : tags;
                emit conversationsChanged(current->path, conversations(current->path));
            });
        return;
    }
}

} // namespace ketplus
