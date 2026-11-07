#include "storage/ReplayDetailCompactor.h"
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

std::optional<QJsonObject> compactCompletedReplayItem(const QJsonObject& original,
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
} // namespace ketplus::daemon
