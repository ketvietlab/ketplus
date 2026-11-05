#include "agent/TranscriptPresentationGrouper.h"

#include "agent/AgentMessageFormatter.h"

namespace ketplus {

QVariantList TranscriptPresentationGrouper::group(const QVariantList& canonicalItems) {
    QVariantList result;
    result.reserve(canonicalItems.size());
    for (const QVariant& value : canonicalItems) {
        const QVariantMap incoming = value.toMap();
        if (!result.isEmpty() && incoming.value(QStringLiteral("eventType")).toString() ==
                                     QStringLiteral("image.view")) {
            QVariantMap previous = result.constLast().toMap();
            const bool sameTurn = previous.value(QStringLiteral("turnId")) ==
                                  incoming.value(QStringLiteral("turnId"));
            if (sameTurn &&
                agent_message::mergeImageActivity(
                    previous, incoming, !incoming.value(QStringLiteral("running")).toBool())) {
                result.last() = previous;
                continue;
            }
        }
        result.append(incoming);
    }
    return result;
}

} // namespace ketplus
