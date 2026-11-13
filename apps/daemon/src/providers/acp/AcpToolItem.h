#pragma once
#include <QJsonArray>
#include <QJsonObject>

namespace ketplus::daemon {
// ACP sends partial tool updates. Preserve the original input and accumulate output.
inline QJsonObject acpToolUpdate(QJsonObject previous, const QJsonObject& update) {
    for (auto it = update.begin(); it != update.end(); ++it) {
        if (it.key() == "content") {
            QJsonArray content = previous.value("content").toArray();
            for (const auto& block : it.value().toArray())
                content.append(block);
            previous.insert(it.key(), content);
        } else
            previous.insert(it.key(), it.value());
    }
    return previous;
}
inline QJsonObject acpToolItem(const QJsonObject& raw) {
    const auto kind = raw.value("kind").toString();
    const auto input = raw.value("rawInput").toObject();
    const auto title = raw.value("title").toString(raw.value("toolCallId").toString());
    const bool command = kind == "execute" || input.contains("command");
    QString tool = kind == "read" ? "read_file" : kind == "search" ? "search_code" : title;
    const auto state = raw.value("status").toString();
    auto metadata = raw;
    metadata.remove("content");
    metadata.remove("rawOutput");
    QJsonObject item{{"id", raw.value("toolCallId")},
                     {"type", command ? "commandExecution" : "dynamicToolCall"},
                     {"tool", tool},
                     {"arguments", input},
                     {"acp", metadata},
                     {"status", state == "pending" || state == "in_progress" || state.isEmpty()
                                    ? "inProgress"
                                    : state}};
    QJsonArray content;
    QStringList output;
    QJsonArray changes;
    for (const auto& value : raw.value("content").toArray()) {
        const auto block = value.toObject();
        if (block.value("type") == "content") {
            const auto nested = block.value("content").toObject();
            content.append(nested);
            if (nested.value("type") == "text")
                output.append(nested.value("text").toString());
        } else if (block.value("type") == "diff") {
            auto oldLines = block.value("oldText").toString().split('\n');
            auto newLines = block.value("newText").toString().split('\n');
            if (!oldLines.isEmpty() && oldLines.last().isEmpty())
                oldLines.removeLast();
            if (!newLines.isEmpty() && newLines.last().isEmpty())
                newLines.removeLast();
            QString diff = QStringLiteral("--- a/%1\n+++ b/%1\n@@ -%2,%3 +%4,%5 @@\n")
                               .arg(block.value("path").toString())
                               .arg(oldLines.isEmpty() ? 0 : 1)
                               .arg(oldLines.size())
                               .arg(newLines.isEmpty() ? 0 : 1)
                               .arg(newLines.size());
            for (const auto& line : oldLines)
                diff += '-' + line + '\n';
            for (const auto& line : newLines)
                diff += '+' + line + '\n';
            changes.append(
                QJsonObject{{"path", block.value("path")}, {"kind", "update"}, {"diff", diff}});
        } else
            content.append(block);
    }
    item.insert("result", QJsonObject{{"content", content},
                                      {"isError", state == "failed"},
                                      {"rawOutput", raw.value("rawOutput")}});
    if (!changes.isEmpty()) {
        item.insert("type", "fileChange");
        item.insert("changes", changes);
    } else if (command) {
        item.insert("command", input.value("command").toString(title));
        item.insert("cwd", input.value("cwd"));
        item.insert("aggregatedOutput", output.join('\n'));
    }
    return item;
}
} // namespace ketplus::daemon
