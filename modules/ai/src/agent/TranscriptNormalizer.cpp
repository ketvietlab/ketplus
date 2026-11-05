#include "agent/TranscriptNormalizer.h"

#include "agent/AgentMessageFormatter.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/McpToolResultMedia.h"

#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace ketplus {

bool TranscriptSanitizer::isSensitiveKey(const QString& key) {
    static const QSet<QString> exact{QStringLiteral("authorization"),
                                     QStringLiteral("cookie"),
                                     QStringLiteral("credentials"),
                                     QStringLiteral("password"),
                                     QStringLiteral("passwd"),
                                     QStringLiteral("secret"),
                                     QStringLiteral("token"),
                                     QStringLiteral("access_token"),
                                     QStringLiteral("refresh_token"),
                                     QStringLiteral("api_key"),
                                     QStringLiteral("apikey"),
                                     QStringLiteral("client_secret"),
                                     QStringLiteral("auth"),
                                     QStringLiteral("code"),
                                     QStringLiteral("key")};
    const QString normalized = QString(key).toLower().replace('-', '_');
    return exact.contains(normalized) || normalized.endsWith(QStringLiteral("_token")) ||
           normalized.endsWith(QStringLiteral("_password")) ||
           normalized.endsWith(QStringLiteral("_secret")) ||
           normalized.endsWith(QStringLiteral("_api_key"));
}

QString TranscriptSanitizer::url(const QString& value) {
    QUrl parsed(value);
    if (!parsed.isValid() || parsed.scheme().isEmpty())
        return value;
    if (!parsed.userName().isEmpty() || !parsed.password().isEmpty()) {
        parsed.setUserName(QStringLiteral("redacted"));
        parsed.setPassword(QStringLiteral("redacted"));
    }
    QUrlQuery query(parsed);
    const auto items = query.queryItems(QUrl::FullyDecoded);
    query.clear();
    for (const auto& item : items) {
        query.addQueryItem(item.first,
                           isSensitiveKey(item.first) ? QStringLiteral("[redacted]") : item.second);
    }
    parsed.setQuery(query);
    if (parsed.hasFragment() && parsed.fragment().contains(QLatin1Char('='))) {
        QUrlQuery fragmentQuery(parsed.fragment(QUrl::FullyDecoded));
        const auto fragmentItems = fragmentQuery.queryItems(QUrl::FullyDecoded);
        fragmentQuery.clear();
        for (const auto& item : fragmentItems) {
            fragmentQuery.addQueryItem(item.first, isSensitiveKey(item.first)
                                                       ? QStringLiteral("[redacted]")
                                                       : item.second);
        }
        parsed.setFragment(fragmentQuery.toString(QUrl::FullyDecoded));
    }
    return parsed.toString(QUrl::FullyEncoded);
}

QString TranscriptSanitizer::text(QString value) {
    // Every string of every restored item comes through here, command output
    // included, so each pattern first needs the text it cannot match without.
    // Compiling "\\s" per call and parsing whole outputs as URLs once made
    // building a history page cost tens of milliseconds.
    static const QRegularExpression whitespace(QStringLiteral("\\s"));
    if (value.contains(QLatin1Char(':')) && !value.contains(whitespace)) {
        const QUrl possibleUrl(value);
        if (possibleUrl.isValid() && !possibleUrl.scheme().isEmpty())
            return url(value);
    }
    static const QRegularExpression assignment(
        QStringLiteral(R"((?i)\b(token|password|secret|api[_-]?key|authorization)=([^\s]+))"));
    static const QRegularExpression option(QStringLiteral(
        R"((?i)(--(?:token|password|secret|api[_-]?key|authorization)\s+)([^\s]+))"));
    static const QRegularExpression bearer(
        QStringLiteral(R"((?i)(\bBearer\s+)([A-Za-z0-9._~+\-/=]+))"));
    if (value.contains(QLatin1Char('=')))
        value.replace(assignment, QStringLiteral("\\1=[redacted]"));
    if (value.contains(QLatin1StringView("--")))
        value.replace(option, QStringLiteral("\\1[redacted]"));
    if (value.contains(QLatin1StringView("bearer"), Qt::CaseInsensitive))
        value.replace(bearer, QStringLiteral("\\1[redacted]"));
    if (!value.contains(QLatin1StringView("://")))
        return value;
    // A label such as "Ran: curl ..." is prose, not a URL with a "ran" scheme.
    // Sanitize embedded URLs separately without percent-encoding the whole label.
    // The scheme starts at the first letter of the scheme-character run before "://". Matching
    // starts only at such a run and never backtracks into it, so long output without "://"
    // stays linear when PCRE2 runs without JIT.
    static const QRegularExpression embeddedUrl(
        QStringLiteral(R"((?<![A-Za-z0-9+.-])[0-9+.-]*+([A-Za-z][A-Za-z0-9+.-]*+://[^\s"'<>]++))"));
    qsizetype offset = 0;
    while (true) {
        const auto match = embeddedUrl.match(value, offset);
        if (!match.hasMatch())
            break;
        const auto safe = url(match.captured(1));
        value.replace(match.capturedStart(1), match.capturedLength(1), safe);
        offset = match.capturedStart(1) + safe.size();
    }
    return value;
}

QJsonValue TranscriptSanitizer::json(const QJsonValue& value, const QString& key) {
    if (isSensitiveKey(key))
        return QStringLiteral("[redacted]");
    if (value.isObject()) {
        QJsonObject result;
        const auto object = value.toObject();
        // MCP image/audio content blocks carry base64 payloads; model rows keep only a marker.
        // Images are materialized separately (McpToolResultMedia) with size limits.
        const QString blockType = object.value(QStringLiteral("type")).toString();
        const bool inlineMedia =
            (blockType == QStringLiteral("image") || blockType == QStringLiteral("audio")) &&
            object.value(QStringLiteral("data")).isString();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (inlineMedia && it.key() == QStringLiteral("data"))
                result.insert(it.key(), QStringLiteral("[inline %1 omitted]").arg(blockType));
            else if (it.key() == QStringLiteral("blob") && it.value().isString())
                // Embedded MCP resource contents ({resource:{uri, mimeType, blob}}).
                result.insert(it.key(), QStringLiteral("[inline resource omitted]"));
            else
                result.insert(it.key(), json(it.value(), it.key()));
        }
        return result;
    }
    if (value.isArray()) {
        QJsonArray result;
        for (const auto& item : value.toArray())
            result.append(json(item));
        return result;
    }
    if (value.isString())
        return text(value.toString());
    return value;
}

TranscriptEvent ActivityNormalizer::normalize(const QJsonObject& item,
                                              const TranscriptPhase lifecycle,
                                              const Context& context) {
    const QString rawType = item.value(QStringLiteral("type")).toString();
    const QString itemId = item.value(QStringLiteral("id")).toString();
    QVariantMap presentation;
    bool visible = true;

    if (rawType == QStringLiteral("userMessage") ||
        rawType == QStringLiteral("daemonUserMessage")) {
        QString textValue = item.value(QStringLiteral("text")).toString();
        QVariantList images;
        for (const auto& value : item.value(QStringLiteral("content")).toArray()) {
            const auto content = value.toObject();
            textValue += content.value(QStringLiteral("text")).toString();
            if (content.value(QStringLiteral("type")) == QStringLiteral("localImage"))
                images.append(QUrl::fromLocalFile(content.value(QStringLiteral("path")).toString())
                                  .toString());
            else if (content.value(QStringLiteral("type")) == QStringLiteral("image"))
                images.append(
                    TranscriptSanitizer::url(content.value(QStringLiteral("url")).toString()));
        }
        // A daemon user.message carries the images it was sent with as media references.
        images.append(mcp_media::messageImages(item.value(QStringLiteral("images")).toArray()));
        presentation = agent_message::message(QStringLiteral("user"), textValue,
                                              QStringLiteral("You"), {}, itemId);
        presentation.insert(QStringLiteral("category"), QStringLiteral("user"));
        presentation.insert(QStringLiteral("eventType"), QStringLiteral("user.message"));
        presentation.insert(QStringLiteral("title"), QStringLiteral("You"));
        presentation.insert(QStringLiteral("images"), images);
        agent_message::markAgentOrigin(presentation,
                                       item.value(QStringLiteral("agentMessages")).toArray());
    } else if (rawType == QStringLiteral("agentMessage")) {
        const QString messagePhase = item.value(QStringLiteral("phase")).toString();
        visible = messagePhase == QStringLiteral("final_answer") ||
                  messagePhase == QStringLiteral("commentary") || messagePhase.isEmpty();
        presentation =
            agent_message::assistantFinalMessage(item, lifecycle == TranscriptPhase::Completed);
        if (messagePhase == QStringLiteral("commentary")) {
            presentation.insert(QStringLiteral("eventType"),
                                QStringLiteral("assistant.commentary"));
            presentation.insert(QStringLiteral("title"), QStringLiteral("Update"));
        }
    } else if (rawType == QStringLiteral("reasoning")) {
        visible = false;
        presentation = agent_message::message(QStringLiteral("tool"), {}, {}, {}, itemId);
        presentation.insert(QStringLiteral("category"), QStringLiteral("system"));
        presentation.insert(QStringLiteral("eventType"), QStringLiteral("assistant.reasoning"));
        presentation.insert(QStringLiteral("subtype"), QStringLiteral("reasoning"));
    } else {
        presentation =
            agent_message::activityMessage(item, lifecycle == TranscriptPhase::Completed);
    }

    TranscriptIdentity identity{context.sessionId, context.generation, context.turnId, itemId,
                                context.attempt,   context.sequence};
    TranscriptEvent event = TranscriptEvent::fromVariantMap(presentation, identity, rawType);
    if (event.eventType.startsWith(QStringLiteral("system.")))
        event.category = TranscriptCategory::System;
    event.phase = lifecycle;
    const QString sourceStatus = item.value(QStringLiteral("status")).toString();
    if (sourceStatus == QStringLiteral("failed") ||
        (lifecycle == TranscriptPhase::Completed &&
         item.value(QStringLiteral("result")).toObject().value(QStringLiteral("isError")).toBool()))
        event.phase = TranscriptPhase::Failed;
    else if (sourceStatus == QStringLiteral("interrupted") ||
             sourceStatus == QStringLiteral("cancelled"))
        event.phase = TranscriptPhase::Interrupted;
    event.visible = visible;
    event.sanitizedPayload = TranscriptSanitizer::json(item).toObject().toVariantMap();
    return event;
}

QList<TranscriptEvent> ActivityNormalizer::normalizeTurn(const QJsonObject& turn,
                                                         const Context& context) {
    QList<TranscriptEvent> result;
    Context itemContext = context;
    itemContext.turnId = turn.value(QStringLiteral("id")).toString();
    const auto items = turn.value(QStringLiteral("items")).toArray();

    QString selectedLegacyAgent;
    bool hasExplicitFinal = false;
    for (const auto& itemValue : items) {
        const auto item = itemValue.toObject();
        if (item.value(QStringLiteral("type")) != QStringLiteral("agentMessage"))
            continue;
        const QString phase = item.value(QStringLiteral("phase")).toString();
        if (phase == QStringLiteral("final_answer"))
            hasExplicitFinal = true;
        else if (phase != QStringLiteral("commentary"))
            selectedLegacyAgent = item.value(QStringLiteral("id")).toString();
    }

    qint64 ordinal = 0;
    for (const auto& itemValue : items) {
        const auto item = itemValue.toObject();
        itemContext.sequence = context.sequence + ordinal++;
        TranscriptEvent event = normalize(item, TranscriptPhase::Completed, itemContext);
        if (event.rawType == QStringLiteral("agentMessage") && !event.visible &&
            !hasExplicitFinal &&
            item.value(QStringLiteral("phase")).toString() != QStringLiteral("commentary") &&
            event.identity.itemId == selectedLegacyAgent) {
            event.visible = true;
            event.category = TranscriptCategory::Final;
            event.eventType = QStringLiteral("assistant.final");
        }
        result.append(std::move(event));
    }
    return result;
}

QSet<QString> ActivityNormalizer::registeredRawTypes() {
    return {QStringLiteral("userMessage"),      QStringLiteral("daemonUserMessage"),
            QStringLiteral("agentMessage"),     QStringLiteral("reasoning"),
            QStringLiteral("commandExecution"), QStringLiteral("fileChange"),
            QStringLiteral("webSearch"),        QStringLiteral("imageView"),
            QStringLiteral("imageGeneration"),  QStringLiteral("mcpToolCall"),
            QStringLiteral("dynamicToolCall"),  QStringLiteral("collabAgentToolCall"),
            QStringLiteral("subAgentActivity"), QStringLiteral("sleep"),
            QStringLiteral("contextCompaction")};
}

QSet<QString> ActivityNormalizer::registeredProtocolEventTypes() {
    return {QStringLiteral("approval.expired"),
            QStringLiteral("approval.requested"),
            QStringLiteral("approval.resolved"),
            QStringLiteral("assistant.delta"),
            QStringLiteral("command.output_delta"),
            QStringLiteral("item.completed"),
            QStringLiteral("item.started"),
            QStringLiteral("provider.protocol_error"),
            QStringLiteral("provider.request_declined"),
            QStringLiteral("provider.runtime"),
            QStringLiteral("runtime.error"),
            QStringLiteral("session.created"),
            QStringLiteral("session.renamed"),
            QStringLiteral("turn.completed"),
            QStringLiteral("turn.queued"),
            QStringLiteral("turn.started"),
            QStringLiteral("turn.steered"),
            QStringLiteral("user.message")};
}

ActivityNormalizer::ProtocolEventPolicy
ActivityNormalizer::protocolPolicy(const QString& eventType) {
    if (eventType == QStringLiteral("item.started") ||
        eventType == QStringLiteral("item.completed") ||
        eventType == QStringLiteral("user.message"))
        return ProtocolEventPolicy::NormalizeItem;
    if (eventType == QStringLiteral("assistant.delta") ||
        eventType == QStringLiteral("command.output_delta"))
        return ProtocolEventPolicy::MergeDelta;
    if (eventType.startsWith(QStringLiteral("approval.")))
        return ProtocolEventPolicy::Approval;
    if (eventType == QStringLiteral("runtime.error"))
        return ProtocolEventPolicy::Error;
    if (eventType.startsWith(QStringLiteral("session.")) ||
        eventType.startsWith(QStringLiteral("turn.")))
        return ProtocolEventPolicy::Metadata;
    return ProtocolEventPolicy::Generic;
}

void TranscriptLifecycleMerger::apply(TranscriptEvent event) {
    const QString key = event.identity.stableKey();
    const auto found = indexByKey_.constFind(key);
    if (found == indexByKey_.constEnd()) {
        indexByKey_.insert(key, events_.size());
        events_.append(std::move(event));
        return;
    }

    TranscriptEvent& current = events_[*found];
    if (event.phase == TranscriptPhase::Delta) {
        current.body += event.body;
        QVariantMap details = current.details;
        const QString output = event.details.value(QStringLiteral("output")).toString();
        if (!output.isEmpty())
            details.insert(QStringLiteral("output"),
                           details.value(QStringLiteral("output")).toString() + output);
        current.details = details;
        current.phase = TranscriptPhase::Delta;
        return;
    }

    const TranscriptIdentity identity = current.identity;
    current = std::move(event);
    current.identity = identity;
}

QVariantList TranscriptLifecycleMerger::variantEvents() const {
    QVariantList result;
    result.reserve(events_.size());
    for (const auto& event : events_) {
        if (event.visible)
            result.append(event.toVariantMap());
    }
    return result;
}

} // namespace ketplus
