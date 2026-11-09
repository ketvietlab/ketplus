#include "agent/DaemonAgentNotes.h"
#include "agent/DaemonTranscriptProjection.h"

namespace ketplus::agent_message {
namespace {
QString stateLabel(const QString& state, const QString& delivery) {
    if (state == QLatin1String("sent") || state == QLatin1String("delivered"))
        return delivery == QLatin1String("queued")
                   ? QStringLiteral("Sent · delivered when its turn ends")
                   : QStringLiteral("Sent");
    if (state == QLatin1String("replied"))
        return QStringLiteral("Replied");
    if (state == QLatin1String("expired"))
        return QStringLiteral("Expired");
    if (state == QLatin1String("held"))
        return QStringLiteral("Held until the turn ends");
    if (state == QLatin1String("authorized") || state == QLatin1String("releasing"))
        return QStringLiteral("Delivering");
    return QStringLiteral("Not delivered");
}

// "DS Icon · Claude · Replied · reply requested", from what the note holds.
void describe(QVariantMap& note) {
    QStringList parts;
    if (const auto title = note.value("agentPeerTitle").toString(); !title.isEmpty())
        parts.append(title);
    if (const auto provider = note.value("agentPeerProvider").toString(); !provider.isEmpty())
        parts.append(provider.left(1).toUpper() + provider.mid(1));
    const auto state = note.value("status").toString();
    parts.append(stateLabel(state, note.value("agentDelivery").toString()));
    if (note.value("agentExpectReply").toBool() && state == QLatin1String("sent"))
        parts.append(QStringLiteral("reply requested"));
    note.insert("meta", parts.join(QStringLiteral(" · ")));
}
} // namespace

QVariantMap agentNote(const QJsonObject& data, const QString& itemId) {
    const auto kind = data.value("kind").toString();
    const auto messageId = data.value("message_id").toString();
    if (data.value("direction").toString() != QLatin1String("outgoing") || messageId.isEmpty() ||
        kind == QLatin1String("review") || data.value("state").toString().startsWith("review_"))
        return {};
    const QString peer = QStringLiteral("@agent/") + data.value("peer_number").toString();
    auto note =
        providerNoticeMessage(data.value("text").toString(), itemId,
                              (kind == QLatin1String("reply")    ? QStringLiteral("Reply to ")
                               : kind == QLatin1String("notice") ? QStringLiteral("Notice to ")
                                                                 : QStringLiteral("Message to ")) +
                                  peer);
    note.insert("subtype", QStringLiteral("agentNote"));
    note.insert("agentMessageId", messageId);
    note.insert("agentPeerTitle", data.value("peer_title").toString());
    note.insert("agentPeerProvider", data.value("peer_provider").toString());
    note.insert("agentExpectReply", data.value("expect_reply").toBool());
    note.insert("agentDelivery", data.value("delivery").toString());
    // In the replier's conversation "replied" is its own answer leaving: it was sent.
    const auto state = data.value("state").toString();
    note.insert("status", state == QLatin1String("replied") ? QStringLiteral("sent") : state);
    describe(note);
    return note;
}

QVariantMap mergedAgentNote(const QVariantMap& note, const QVariantMap& update) {
    auto merged = note;
    merged.insert("status", update.value("status"));
    for (const char* key : {"agentPeerTitle", "agentPeerProvider", "agentDelivery"})
        if (!update.value(key).toString().isEmpty())
            merged.insert(key, update.value(key));
    if (!update.value("body").toString().isEmpty()) {
        merged.insert("body", update.value("body"));
        merged.insert("summary", update.value("body"));
    }
    // A reply has arrived; a later state of the request does not undo it.
    if (note.value("status") == QLatin1String("replied"))
        merged.insert("status", note.value("status"));
    describe(merged);
    return merged;
}

QVariantMap repliedAgentNote(const QVariantMap& note) {
    auto replied = note;
    replied.insert("status", QStringLiteral("replied"));
    describe(replied);
    return replied;
}

QStringList agentRepliesTo(const QJsonArray& agentMessages) {
    QStringList ids;
    for (const auto& value : agentMessages) {
        const auto message = value.toObject();
        if (message.value("kind").toString() == QLatin1String("reply") &&
            !message.value("reply_to").toString().isEmpty())
            ids.append(message.value("reply_to").toString());
    }
    return ids;
}

} // namespace ketplus::agent_message
