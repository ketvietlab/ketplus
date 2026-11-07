#include "storage/ReplayDetailCompactor.h"
#include "agent/FileChangeStatistics.h"
#include "agent/TranscriptSanitizer.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>

namespace ketplus::daemon {
namespace {
QByteArray prefix(const QByteArray& bytes, qsizetype budget) {
    qsizetype end = std::min(bytes.size(), budget);
    while (end > 0 && end < bytes.size() &&
           (static_cast<unsigned char>(bytes.at(end)) & 0xc0) == 0x80)
        --end;
    return bytes.first(end);
}
QJsonObject marker(const QDateTime& cutoff, const QString& policy, qint64 original,
                   qint64 retained) {
    return {{"at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"cutoff", cutoff.toUTC().toString(Qt::ISODateWithMs)},
            {"policy", policy},
            {"original_bytes", original},
            {"retained_bytes", retained}};
}
bool isText(const QJsonValue& value) {
    const auto block = value.toObject();
    const auto type = block.value("type").toString();
    return (type == "text" || type == "inputText") && block.value("text").isString();
}
qint64 textBytes(const QJsonArray& blocks) {
    qint64 bytes = 0;
    for (const auto& block : blocks)
        if (isText(block))
            bytes += block.toObject().value("text").toString().toUtf8().size();
    return bytes;
}
QJsonArray trimText(const QJsonArray& blocks, qsizetype& budget, qint64& retained) {
    QJsonArray result;
    bool noticeInserted = false;
    for (const auto& value : blocks) {
        if (!isText(value)) {
            result.append(value);
            continue;
        }
        auto block = value.toObject();
        const auto bytes = block.value("text").toString().toUtf8();
        const auto kept = prefix(bytes, budget);
        budget -= kept.size();
        retained += kept.size();
        if (kept.size() < bytes.size())
            budget = 0; // a preview is a prefix, never skip ahead to later blocks
        QString text = QString::fromUtf8(kept);
        if (!noticeInserted) {
            text.prepend(QStringLiteral("[Tool result trimmed · history retention]\n"));
            noticeInserted = true;
        }
        block.insert("text", text);
        result.append(block); // preserve annotations, audience and block ordering
    }
    return result;
}
bool hasSummary(const QJsonValue& value) {
    if (value.isString())
        return !value.toString().trimmed().isEmpty();
    for (const auto& part : value.toArray())
        if ((part.isString() && !part.toString().trimmed().isEmpty()) ||
            !part.toObject().value("text").toString().trimmed().isEmpty())
            return true;
    return false;
}
} // namespace

static std::optional<QJsonObject> compactContent(const QJsonObject& original,
                                                 const QDateTime& cutoff) {
    if (!cutoff.isValid() || cutoff > QDateTime::currentDateTimeUtc() ||
        original.contains("compacted"))
        return std::nullopt;
    auto item = original;
    const auto type = item.value("type").toString();
    if (type == "reasoning") {
        if (item.contains("status") && item.value("status") != "completed" &&
            item.value("status") != "failed")
            return std::nullopt;
        const auto raw = item.value("content");
        if (!hasSummary(item.value("summary")) || (!raw.isString() && !raw.isArray()))
            return std::nullopt;
        for (const auto& part : raw.toArray())
            if (!part.isString())
                return std::nullopt; // unknown structured content may own media/resources
        const auto bytes = raw.isString()
                               ? raw.toString().toUtf8()
                               : QJsonDocument(raw.toArray()).toJson(QJsonDocument::Compact);
        if (bytes.size() <= 1024)
            return std::nullopt;
        item.insert("content", raw.isArray() ? QJsonValue(QJsonArray{}) : QJsonValue(QString{}));
        item.insert("compacted", marker(cutoff, "reasoning-content-v1", bytes.size(), 0));
        return item;
    }
    const auto status = item.value("status").toString();
    if (status != "completed" && status != "failed")
        return std::nullopt;
    if (type == "fileChange") {
        auto changes = item.value("changes").toArray();
        qint64 originalBytes = 0;
        qint64 retainedBytes = 0;
        for (qsizetype i = 0; i < changes.size(); ++i) {
            auto change = changes.at(i).toObject();
            if (!change.value("diff").isString() || change.contains("compacted") ||
                change.contains("diff_excerpt"))
                continue;
            const auto diff = change.value("diff").toString().toUtf8();
            if (diff.size() <= 64 * 1024)
                continue;
            const auto stats = rawFileChangeStatistics(change);
            // Leave margin for metadata while keeping the excerpt under the 64 KiB cap.
            const auto excerpt = prefix(diff, 63 * 1024);
            auto info = marker(cutoff, "file-diff-v1", diff.size(), excerpt.size());
            info.insert("stats", QJsonObject{{"added", stats.added},
                                             {"removed", stats.removed},
                                             {"known", stats.known}});
            info.insert("excerpt_only", true);
            change.remove("diff"); // legacy readers must not mistake a prefix for a complete patch
            change.insert("diff_excerpt", QString::fromUtf8(excerpt));
            change.insert("compacted", info);
            changes.replace(i, change);
            originalBytes += diff.size();
            retainedBytes += excerpt.size();
        }
        if (originalBytes == 0)
            return std::nullopt;
        item.insert("changes", changes);
        item.insert("compacted", marker(cutoff, "file-diff-v1", originalBytes, retainedBytes));
        return item;
    }
    if (type == "commandExecution") {
        const auto output = item.value("aggregatedOutput").toString().toUtf8();
        if (output.size() <= 21 * 1024)
            return std::nullopt;
        const auto head = prefix(output, 4 * 1024);
        qsizetype tail = output.size() - 16 * 1024;
        while (tail < output.size() && (static_cast<unsigned char>(output.at(tail)) & 0xc0) == 0x80)
            ++tail;
        item.insert(
            "aggregatedOutput",
            QStringLiteral("[Output trimmed · original size %1 bytes · history retention]\n")
                    .arg(output.size()) +
                QString::fromUtf8(head) + QStringLiteral("\n[Middle output removed]\n") +
                QString::fromUtf8(output.sliced(tail)));
        item.insert("compacted", marker(cutoff, "command-output-v1", output.size(),
                                        head.size() + output.size() - tail));
        return item;
    }
    if (type != "mcpToolCall" && type != "dynamicToolCall")
        return std::nullopt;
    const auto result = item.value("result");
    const auto blocks = result.toObject().value("content").toArray();
    const auto contentItems = item.value("contentItems").toArray();
    const auto resultBytes = result.isString() ? result.toString().toUtf8() : QByteArray{};
    const qint64 originalBytes = textBytes(blocks) + textBytes(contentItems) + resultBytes.size();
    if (originalBytes <= 5 * 1024)
        return std::nullopt;
    qsizetype budget = 4 * 1024;
    qint64 retained = 0;
    if (result.isString()) {
        const auto kept = prefix(resultBytes, budget);
        budget -= kept.size();
        retained += kept.size();
        if (kept.size() < resultBytes.size())
            budget = 0;
        item.insert("result", QStringLiteral("[Tool result trimmed · history retention]\n") +
                                  QString::fromUtf8(kept));
    } else if (!blocks.isEmpty()) {
        auto transformed = result.toObject();
        transformed.insert("content", trimText(blocks, budget, retained));
        item.insert("result", transformed);
    }
    if (!contentItems.isEmpty())
        item.insert("contentItems", trimText(contentItems, budget, retained));
    item.insert("compacted", marker(cutoff, "tool-result-text-v1", originalBytes, retained));
    return item;
}

static std::optional<QJsonObject> compactArguments(const QJsonObject& original,
                                                   const QDateTime& cutoff) {
    const auto content = compactContent(original, cutoff);
    auto item = content.value_or(original);
    const auto type = item.value("type").toString();
    const auto status = item.value("status").toString();
    auto info = item.value("compacted").toObject();
    // Only extend our known text-only marker. Unknown/future policies remain immutable.
    if (!cutoff.isValid() || cutoff > QDateTime::currentDateTimeUtc() ||
        (type != "mcpToolCall" && type != "dynamicToolCall") ||
        (status != "completed" && status != "failed") || item.contains("arguments_excerpt") ||
        (item.contains("compacted") &&
         (info.value("policy") != "tool-result-text-v1" || info.contains("arguments"))))
        return content;
    const auto arguments = item.value("arguments");
    QByteArray bytes;
    if (arguments.isString())
        bytes = arguments.toString().toUtf8();
    else if (arguments.isObject())
        bytes = QJsonDocument(arguments.toObject()).toJson(QJsonDocument::Compact);
    else if (arguments.isArray())
        bytes = QJsonDocument(arguments.toArray()).toJson(QJsonDocument::Compact);
    // Leave room for the explicit notice and marker instead of growing small records.
    if (bytes.size() <= 5 * 1024)
        return content;
    const qint64 originalBytes = bytes.size();
    auto safeArguments = TranscriptSanitizer::json(arguments);
    // Dynamic tools may encode their argument object as a JSON string.
    if (arguments.isString()) {
        const auto parsed = QJsonDocument::fromJson(bytes);
        if (parsed.isObject())
            safeArguments = TranscriptSanitizer::json(parsed.object());
        else if (parsed.isArray())
            safeArguments = TranscriptSanitizer::json(parsed.array());
    }
    const auto safeBytes =
        safeArguments.isString() ? safeArguments.toString().toUtf8()
        : safeArguments.isArray()
            ? QJsonDocument(safeArguments.toArray()).toJson(QJsonDocument::Compact)
            : QJsonDocument(safeArguments.toObject()).toJson(QJsonDocument::Compact);
    const auto excerpt = prefix(safeBytes, 4 * 1024);
    auto argumentInfo = marker(cutoff, "tool-arguments-v1", originalBytes, excerpt.size());
    argumentInfo.insert("excerpt_only", true);
    argumentInfo.insert("redacted_before_excerpt", true);
    argumentInfo.insert("format", arguments.isString() ? "text" : "json");
    if (info.isEmpty())
        info = marker(cutoff, "tool-arguments-v1", originalBytes, excerpt.size());
    info.insert("arguments", argumentInfo);
    // A JSON prefix is not a valid argument object. Never expose it as the original call input.
    item.remove("arguments");
    item.insert("arguments_excerpt",
                QStringLiteral("[Arguments trimmed · original size %1 bytes · history retention]\n")
                        .arg(bytes.size()) +
                    QString::fromUtf8(excerpt));
    item.insert("compacted", info);
    return item;
}
namespace {
// Structured results may embed resource ownership. Keep those records until the
// separate media-retention policy can preserve their references and previews.
bool plainData(const QJsonValue& value, int depth = 0) {
    if (depth > 64)
        return false;
    if (value.isObject()) {
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const auto key = it.key().toLower();
            if (key == "type" || key == "uri" || key == "url" || key == "path" ||
                key == "mimetype" || key == "blob" || key == "data" || key.contains("media") ||
                key.contains("resource") || key.contains("ref"))
                return false;
            if (!plainData(it.value(), depth + 1))
                return false;
        }
    } else if (value.isArray()) {
        for (const auto& part : value.toArray())
            if (!plainData(part, depth + 1))
                return false;
    } else if (value.isString()) {
        const auto text = value.toString();
        if (text.contains("data:", Qt::CaseInsensitive) ||
            text.contains("file:", Qt::CaseInsensitive))
            return false;
    }
    return true;
}
} // namespace

std::optional<QJsonObject> compactCompletedReplayItem(const QJsonObject& original,
                                                      const QDateTime& cutoff) {
    const auto earlier = compactArguments(original, cutoff);
    auto item = earlier.value_or(original);
    auto info = item.value("compacted").toObject();
    const auto policy = info.value("policy").toString();
    auto result = item.value("result").toObject();
    const auto structured = result.value("structuredContent");
    if (!cutoff.isValid() || cutoff > QDateTime::currentDateTimeUtc() ||
        (item.value("type") != "mcpToolCall" && item.value("type") != "dynamicToolCall") ||
        item.value("status") != "completed" ||
        (!item.value("error").isUndefined() && !item.value("error").isNull()) ||
        (result.contains("isError") && result.value("isError") != QJsonValue(false)) ||
        result.contains("structuredContent_excerpt") || info.contains("structured_result") ||
        (item.contains("compacted") && policy != "tool-result-text-v1" &&
         policy != "tool-arguments-v1") ||
        (!structured.isObject() && !structured.isArray()) || !plainData(structured))
        return earlier;
    const auto serialize = [](const QJsonValue& value) {
        return value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                               : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    };
    const auto bytes = serialize(structured);
    if (bytes.size() <= 5 * 1024)
        return earlier;
    // Share the result preview allowance with retained text; arguments have their
    // own separate input allowance. Existing notices conservatively consume space.
    const qint64 visibleText = textBytes(result.value("content").toArray()) +
                               textBytes(item.value("contentItems").toArray());
    const auto budget = std::max<qint64>(0, 4 * 1024 - visibleText);
    const auto excerpt = prefix(serialize(TranscriptSanitizer::json(structured)), budget);
    auto details = marker(cutoff, "tool-structured-result-v1", bytes.size(), excerpt.size());
    details.insert("excerpt_only", true);
    details.insert("redacted_before_excerpt", true);
    if (info.isEmpty())
        info = details;
    info.insert("structured_result", details);
    result.remove("structuredContent");
    result.insert("structuredContent_excerpt",
                  QStringLiteral("[Structured result trimmed · history retention]\n") +
                      QString::fromUtf8(excerpt));
    item.insert("result", result);
    item.insert("compacted", info);
    return item;
}
} // namespace ketplus::daemon
