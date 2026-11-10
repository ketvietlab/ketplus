#include "cloud/CloudEventProjection.h"
#include "agents/WorkReport.h"
#include "storage/TranscriptItemSnapshot.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUrl>

namespace ketplus::daemon {
namespace {

void copyIfPresent(const QJsonObject& source, QJsonObject& target, const QString& key) {
    const auto found = source.constFind(key);
    if (found != source.constEnd()) {
        target.insert(key, found.value());
    }
}

bool isCommandOutputEventType(const QString& eventType) {
    const QString normalized = eventType.toLower();
    return normalized.contains(QStringLiteral("command")) &&
           (normalized.contains(QStringLiteral("output")) ||
            normalized.contains(QStringLiteral("stdout")) ||
            normalized.contains(QStringLiteral("stderr")));
}

QString normalizedMarker(QString value) {
    value.removeIf([](QChar character) { return !character.isLetterOrNumber(); });
    return value.toLower();
}

bool hasCommandMetadata(const QJsonObject& payload, const QJsonObject& item) {
    return payload.value(QStringLiteral("command")).isString() ||
           item.value(QStringLiteral("command")).isString();
}

bool hasOutputArtifact(const QJsonObject& payload, const QJsonObject& item) {
    const QSet<QString> sensitive{QStringLiteral("output"), QStringLiteral("aggregatedoutput"),
                                  QStringLiteral("stdout"), QStringLiteral("stderr"),
                                  QStringLiteral("body"),   QStringLiteral("data"),
                                  QStringLiteral("delta")};
    for (const QJsonObject& source : {payload, item}) {
        for (auto field = source.constBegin(); field != source.constEnd(); ++field) {
            if (sensitive.contains(normalizedMarker(field.key()))) {
                return true;
            }
        }
    }
    return false;
}

bool isCommandExecutionMarker(const QString& value, const QJsonObject& payload,
                              const QJsonObject& item) {
    const QString marker = normalizedMarker(value);
    const QSet<QString> knownNonCommand{
        QStringLiteral("agentmessage"), QStringLiteral("usermessage"),
        QStringLiteral("reasoning"),    QStringLiteral("contextcompaction"),
        QStringLiteral("mcptoolcall"),  QStringLiteral("dynamictoolcall"),
        QStringLiteral("websearch"),    QStringLiteral("filechange")};
    return marker == QStringLiteral("commandexecution") ||
           marker == QStringLiteral("shellcommand") ||
           marker == QStringLiteral("terminalcommand") ||
           (marker == QStringLiteral("toolexecution") && hasCommandMetadata(payload, item)) ||
           (!knownNonCommand.contains(marker) && hasCommandMetadata(payload, item) &&
            hasOutputArtifact(payload, item));
}

constexpr qsizetype maximumCommandDescription = 160;
constexpr qsizetype maximumToolPath = 512;

// The agent's one-line account of what a command does, which a phone shows instead of
// the raw shell text.
void copyDescription(const QJsonObject& source, QJsonObject& target) {
    const QString description = source.value(QStringLiteral("description")).toString().trimmed();
    if (!description.isEmpty())
        target.insert(QStringLiteral("description"),
                      capCloudText(sanitizeCloudText(description), maximumCommandDescription));
}

QJsonObject commandProjection(const QJsonObject& payload) {
    QJsonObject projected;
    for (const QString& key :
         {QStringLiteral("item_id"), QStringLiteral("item_type"), QStringLiteral("command"),
          QStringLiteral("status"), QStringLiteral("exitCode")}) {
        copyIfPresent(payload, projected, key);
    }
    const QJsonObject item = payload.value(QStringLiteral("item")).toObject();
    QJsonObject projectedItem;
    for (const QString& key :
         {QStringLiteral("id"), QStringLiteral("type"), QStringLiteral("command"),
          QStringLiteral("status"), QStringLiteral("exitCode")}) {
        copyIfPresent(item, projectedItem, key);
    }
    copyDescription(item, projectedItem);
    if (!projectedItem.isEmpty()) {
        projected.insert(QStringLiteral("item"), projectedItem);
    }
    return projected;
}

constexpr qsizetype maximumMcpArgumentValue = 80;
constexpr qsizetype maximumMcpArgumentSummary = 160;
constexpr qsizetype maximumMcpResultText = 256;

constexpr qsizetype maximumMcpContentBlocks = 32;

// Keys whose values are free text (typed input, scripts, request bodies) or credentials. The
// summary is metadata for a remote client, so anything that can carry user data is dropped.
bool isSensitiveArgumentKey(const QString& key) {
    const QString marker = normalizedMarker(key);
    static const QSet<QString> freeText{
        QStringLiteral("text"),     QStringLiteral("value"),   QStringLiteral("values"),
        QStringLiteral("script"),   QStringLiteral("code"),    QStringLiteral("body"),
        QStringLiteral("content"),  QStringLiteral("data"),    QStringLiteral("expression"),
        QStringLiteral("function"), QStringLiteral("html"),    QStringLiteral("input"),
        QStringLiteral("prompt"),   QStringLiteral("message"), QStringLiteral("source"),
        QStringLiteral("key"),      QStringLiteral("keys"),    QStringLiteral("id"),
        // File edits: the replaced and replacement text are file contents, not metadata.
        QStringLiteral("oldstring"), QStringLiteral("newstring"), QStringLiteral("newsource")};
    if (freeText.contains(marker))
        return true;
    for (const auto* fragment : {"token", "secret", "password", "passwd", "auth", "cookie",
                                 "credential", "apikey", "privatekey", "session", "header"}) {
        if (marker.contains(QLatin1String(fragment)))
            return true;
    }
    return marker.endsWith(QStringLiteral("key")) || key.endsWith(QStringLiteral("Id")) ||
           key.endsWith(QStringLiteral("Ref"));
}

} // namespace

QString capCloudText(const QString& value, const qsizetype limit) {
    if (value.size() <= limit)
        return value;
    qsizetype cut = qMax<qsizetype>(0, limit - 1);
    // Never leave a lone high surrogate at the cut.
    if (cut > 0 && value.at(cut - 1).isHighSurrogate())
        --cut;
    return value.left(cut) + QChar(0x2026);
}

// A path keeps its end: the file name is what a phone shows, and a head-first cut is
// exactly what drops it from a deep repository path.
static QString capCloudPath(const QString& value, const qsizetype limit) {
    if (value.size() <= limit)
        return value;
    qsizetype start = value.size() - qMax<qsizetype>(0, limit - 1);
    if (start < value.size() && value.at(start).isLowSurrogate())
        ++start;
    return QChar(0x2026) + value.mid(start);
}

static bool isPathArgumentKey(const QString& key) {
    const QString marker = normalizedMarker(key);
    return marker == QStringLiteral("path") || marker.endsWith(QStringLiteral("filepath")) ||
           marker == QStringLiteral("notebookpath");
}

QString sanitizeCloudUrl(const QString& value) {
    QUrl url(value);
    if (!url.isValid() || url.scheme().isEmpty())
        return value;
    // Queries and fragments routinely carry tokens (OAuth codes, signed URLs); keep the origin
    // and path only.
    url.setUserInfo({});
    url.setQuery(QString());
    url.setFragment(QString());
    return url.toString(QUrl::FullyEncoded | QUrl::RemoveQuery | QUrl::RemoveFragment);
}

QString sanitizeCloudText(QString value) {
    value.removeIf([](QChar character) { return character.category() == QChar::Other_Control; });
    static const QRegularExpression whitespace(QStringLiteral("\\s"));
    const QUrl possibleUrl(value);
    if (possibleUrl.isValid() && !possibleUrl.scheme().isEmpty() &&
        value.contains(QStringLiteral("://")) && !value.contains(whitespace))
        return sanitizeCloudUrl(value);
    // Same redactions as the desktop TranscriptSanitizer::text.
    static const QRegularExpression assignment(QStringLiteral(
        R"((?i)\b(token|password|passwd|secret|api[_-]?key|authorization|cookie)=([^\s&]+))"));
    static const QRegularExpression option(QStringLiteral(
        R"((?i)(--(?:token|password|secret|api[_-]?key|authorization)\s+)([^\s]+))"));
    static const QRegularExpression bearer(
        QStringLiteral(R"((?i)(\b(?:Bearer|Basic)\s+)([A-Za-z0-9._~+\-/=]+))"));
    value.replace(assignment, QStringLiteral("\\1=[redacted]"));
    value.replace(option, QStringLiteral("\\1[redacted]"));
    value.replace(bearer, QStringLiteral("\\1[redacted]"));
    // The scheme is the run of scheme characters before "://", from its first letter. Matching
    // starts only where such a run starts and never backtracks into it, so a long run without
    // "://" costs linear time even when PCRE2 has no JIT (a hardened-runtime process).
    static const QRegularExpression embeddedUrl(
        QStringLiteral(R"((?<![A-Za-z0-9+.-])[0-9+.-]*+([A-Za-z][A-Za-z0-9+.-]*+://[^\s"'<>]++))"));
    qsizetype offset = 0;
    while (true) {
        const auto match = embeddedUrl.match(value, offset);
        if (!match.hasMatch())
            break;
        const QString safe = sanitizeCloudUrl(match.captured(1));
        value.replace(match.capturedStart(1), match.capturedLength(1), safe);
        offset = match.capturedStart(1) + safe.size();
    }
    return value;
}

QString cloudArgumentSummary(const QJsonObject& arguments) {
    QStringList parts;
    for (auto field = arguments.constBegin(); field != arguments.constEnd(); ++field) {
        const QJsonValue value = field.value();
        if (isSensitiveArgumentKey(field.key()) ||
            !(value.isString() || value.isDouble() || value.isBool()))
            continue;
        const QString text =
            value.isString() ? sanitizeCloudText(value.toString()) : value.toVariant().toString();
        parts.append(sanitizeCloudText(field.key()) + QLatin1Char('=') +
                     (isPathArgumentKey(field.key()) ? capCloudPath(text, maximumMcpArgumentValue)
                                                     : capCloudText(text, maximumMcpArgumentValue)));
    }
    return capCloudText(parts.join(QStringLiteral(" · ")), maximumMcpArgumentSummary);
}

namespace {

// MCP tool calls keep tool identity, status, a sanitized argument summary, media references and
// a short text excerpt. Inline media (`data`), structured content and long text are dropped.
QJsonObject mcpToolCallProjection(const QJsonObject& payload) {
    QJsonObject projected;
    for (const QString& key :
         {QStringLiteral("item_id"), QStringLiteral("item_type"), QStringLiteral("status")}) {
        copyIfPresent(payload, projected, key);
    }
    const QJsonObject item = payload.value(QStringLiteral("item")).toObject();
    QJsonObject projectedItem;
    for (const QString& key :
         {QStringLiteral("id"), QStringLiteral("type"), QStringLiteral("server"),
          QStringLiteral("tool"), QStringLiteral("status"), QStringLiteral("durationMs")}) {
        copyIfPresent(item, projectedItem, key);
    }
    const QString summary =
        cloudArgumentSummary(item.value(QStringLiteral("arguments")).toObject());
    if (!summary.isEmpty())
        projectedItem.insert(QStringLiteral("arguments_summary"), summary);
    // Preserve explicit paths for daemon-verified resource resolution on the phone.
    const QString path = item.value(QStringLiteral("path")).toString();
    if (!path.isEmpty() && path.size() <= maximumToolPath &&
        !path.contains(QLatin1Char('\\')) &&
        !path.split(QLatin1Char('/')).contains(QStringLiteral("..")))
        projectedItem.insert(QStringLiteral("path"), sanitizeCloudText(path));
    if (item.value(QStringLiteral("outsideWorktree")).toBool())
        projectedItem.insert(QStringLiteral("outsideWorktree"), true);
    const QJsonObject result = item.value(QStringLiteral("result")).toObject();
    if (!result.isEmpty()) {
        QJsonObject projectedResult;
        copyIfPresent(result, projectedResult, QStringLiteral("isError"));
        QJsonArray blocks;
        qsizetype textBudget = maximumMcpResultText;
        for (const QJsonValue& value : result.value(QStringLiteral("content")).toArray()) {
            if (blocks.size() >= maximumMcpContentBlocks) {
                projectedResult.insert(QStringLiteral("content_truncated"), true);
                break;
            }
            const QJsonObject block = value.toObject();
            const QString type = block.value(QStringLiteral("type")).toString();
            QJsonObject projectedBlock{{QStringLiteral("type"), type}};
            if (block.value(QStringLiteral("media_ref")).isString()) {
                for (const QString& key :
                     {QStringLiteral("media_ref"), QStringLiteral("mime"), QStringLiteral("bytes"),
                      QStringLiteral("width"), QStringLiteral("height")}) {
                    copyIfPresent(block, projectedBlock, key);
                }
            } else if (type == QStringLiteral("text") && textBudget > 0) {
                const QString text =
                    sanitizeCloudText(block.value(QStringLiteral("text")).toString());
                projectedBlock.insert(QStringLiteral("text"), capCloudText(text, textBudget));
                if (text.size() > textBudget)
                    projectedBlock.insert(QStringLiteral("truncated"), true);
                textBudget -= qMin(text.size(), textBudget);
            } else if (block.value(QStringLiteral("omitted")).toBool()) {
                copyIfPresent(block, projectedBlock, QStringLiteral("omitted"));
                copyIfPresent(block, projectedBlock, QStringLiteral("reason"));
            }
            blocks.append(projectedBlock);
        }
        if (!blocks.isEmpty())
            projectedResult.insert(QStringLiteral("content"), blocks);
        projectedItem.insert(QStringLiteral("result"), projectedResult);
    }
    const QJsonValue error = item.value(QStringLiteral("error"));
    const QString errorMessage = error.isObject()
                                     ? error.toObject().value(QStringLiteral("message")).toString()
                                     : error.toString();
    if (!errorMessage.isEmpty())
        projectedItem.insert(QStringLiteral("error"),
                             capCloudText(sanitizeCloudText(errorMessage), maximumMcpResultText));
    projected.insert(QStringLiteral("item"), projectedItem);
    return projected;
}

} // namespace

QJsonObject projectEventPayloadForCloud(const QString& eventType, const QJsonObject& payload) {
    const QJsonObject item = payload.value(QStringLiteral("item")).toObject();
    const QString itemType =
        payload.value(QStringLiteral("item_type"))
            .toString(payload.value(QStringLiteral("type"))
                          .toString(item.value(QStringLiteral("type")).toString()));
    // Event names are provider-extensible. Any command-associated output/stdout/stderr
    // variant is metadata-only even when its exact spelling is unknown to this build.
    if (eventType == "question.answered") {
        auto lifecycle = payload;
        lifecycle.remove("answers");
        lifecycle.remove("questions");
        return lifecycle;
    }
    if (isCommandOutputEventType(eventType)) {
        return commandProjection(payload);
    }
    // ADR 0106: report text is agent-written but may quote sensitive output.
    if (eventType == QStringLiteral("work_report.updated"))
        return work_report::cloudProjection(payload);
    if ((normalizedMarker(itemType) == QStringLiteral("mcptoolcall") ||
         normalizedMarker(itemType) == QStringLiteral("dynamictoolcall"))) {
        return mcpToolCallProjection(payload);
    }
    if (!isCommandExecutionMarker(itemType, payload, item)) {
        return payload;
    }
    return commandProjection(payload);
}

QJsonObject cloudItemMetadata(const QString& sessionId, const qint64 generation,
                              const QString& eventType, const QJsonObject& payload) {
    const bool delta = eventType == QStringLiteral("assistant.delta") ||
                       eventType == QStringLiteral("command.output_delta");
    const bool completed = eventType == QStringLiteral("item.completed");
    if (!delta && !completed)
        return {};
    const QJsonObject item = payload.value(QStringLiteral("item")).toObject();
    const QString itemId = payload.value(QStringLiteral("item_id"))
                               .toString(item.value(QStringLiteral("id")).toString());
    if (itemId.isEmpty())
        return {};
    const QByteArray digest =
        QCryptographicHash::hash(QStringLiteral("ketplus.cloud-item-key.v1\n%1\n%2\n%3")
                                     .arg(sessionId)
                                     .arg(generation)
                                     .arg(itemId)
                                     .toUtf8(),
                                 QCryptographicHash::Sha256)
            .left(24);
    QJsonObject metadata{{QStringLiteral("itemKey"),
                          QString::fromLatin1(digest.toBase64(QByteArray::Base64UrlEncoding |
                                                              QByteArray::OmitTrailingEquals))}};
    // The local journal's rule, less the large-item snapshot: its text reaches the phone only
    // through the daemon, so the streamed copy stays for when the Mac is away. A command's
    // streamed output is metadata-only in the cloud, so its completion supersedes it.
    if (completed && (hasAuthoritativeAssistantText(payload) ||
                      (item.value(QStringLiteral("type")) == QStringLiteral("commandExecution") &&
                       item.value(QStringLiteral("aggregatedOutput")).isString())))
        metadata.insert(QStringLiteral("supersedesDeltas"), true);
    return metadata;
}

QJsonObject projectTranscriptWindowForCloud(const QJsonObject& page,
                                            const LargeEventCloudProjection& large) {
    QHash<QString, std::optional<QJsonObject>> decisions;
    const auto decide = [&](const QString& transferId,
                            const QJsonObject& manifest) -> const std::optional<QJsonObject>& {
        auto found = decisions.find(transferId);
        if (found == decisions.end()) {
            std::optional<QJsonObject> decision =
                large ? large(transferId, manifest)
                      : std::optional<QJsonObject>{
                            QJsonObject{{QStringLiteral("cloud_omitted"), true}}};
            found = decisions.insert(transferId, decision);
        }
        return found.value();
    };
    QJsonObject projected = page;
    QJsonArray events;
    for (const QJsonValue& value : page.value(QStringLiteral("events")).toArray()) {
        QJsonObject event = value.toObject();
        const QString eventType = event.value(QStringLiteral("event_type")).toString();
        const QJsonObject payload = event.value(QStringLiteral("payload")).toObject();
        if (eventType == QStringLiteral("transcript.large_chunk")) {
            // Raw chunk bytes are only served when the reassembled item is cloud-safe.
            const QString transferId = payload.value(QStringLiteral("transfer_id")).toString();
            if (transferId.isEmpty() || decide(transferId, {}).has_value())
                continue;
            events.append(event);
            continue;
        }
        const QJsonObject manifest = payload.value(QStringLiteral("large_event")).toObject();
        if (!manifest.isEmpty()) {
            const QString transferId = manifest.value(QStringLiteral("transfer_id")).toString();
            const std::optional<QJsonObject>& decision =
                transferId.isEmpty() ? std::optional<QJsonObject>{QJsonObject{
                                           {QStringLiteral("cloud_omitted"), true}}}
                                     : decide(transferId, manifest);
            if (decision.has_value())
                event.insert(QStringLiteral("payload"), *decision);
            events.append(event);
            continue;
        }
        event.insert(QStringLiteral("payload"), projectEventPayloadForCloud(eventType, payload));
        events.append(event);
    }
    projected.insert(QStringLiteral("events"), events);
    QJsonArray partialItems;
    for (const QJsonValue& value : page.value(QStringLiteral("partial_items")).toArray()) {
        const QString transferId = value.toObject().value(QStringLiteral("transfer_id")).toString();
        if (!transferId.isEmpty() &&
            (!decisions.contains(transferId) || decisions.value(transferId).has_value()))
            continue; // inlined (or unverifiable): no continuation to offer
        partialItems.append(value);
    }
    projected.insert(QStringLiteral("partial_items"), partialItems);
    return projected;
}

} // namespace ketplus::daemon
