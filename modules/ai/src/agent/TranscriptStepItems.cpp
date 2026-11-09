#include "agent/TranscriptStepItems.h"

#include <QJsonArray>
#include <QStringList>
#include <QUrl>
#include <algorithm>
#include <utility>

namespace ketplus::agent_message {
namespace {

QJsonObject toolArguments(const QJsonObject& item) {
    const auto arguments = item.value(QStringLiteral("arguments")).toObject();
    return arguments.isEmpty() ? item.value(QStringLiteral("input")).toObject() : arguments;
}

QString firstString(const QJsonObject& object, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const QString value = object.value(QLatin1String(key)).toString().simplified();
        if (!value.isEmpty())
            return value;
    }
    return {};
}

// A page as the mock writes it: host and path, without the scheme or a trailing slash.
QString pageText(const QString& url) {
    const QUrl parsed(url);
    if (!parsed.isValid() || parsed.host().isEmpty())
        return url;
    QString path = parsed.path();
    if (path == QLatin1String("/"))
        path.clear();
    return parsed.host() + path;
}

QString lines(const QString& text, QChar mark) {
    if (text.isEmpty())
        return {};
    QStringList split = text.split(QLatin1Char('\n'));
    // A final newline ends the last line; it does not start another.
    if (text.endsWith(QLatin1Char('\n')))
        split.removeLast();
    QStringList marked;
    for (const auto& line : std::as_const(split))
        marked.append(mark + line);
    return marked.join(QLatin1Char('\n'));
}

// A replacement made once reads as one hunk, so the shared statistics count its lines.
// `replace_all` may replace any number of times: without the hunk header the lines show
// but the counts stay unknown.
QString replacementHunk(const QJsonObject& edit, bool counted = true) {
    const QString removed = lines(edit.value(QStringLiteral("old_string")).toString(), u'-');
    const QString added = lines(edit.value(QStringLiteral("new_string")).toString(), u'+');
    QStringList hunk;
    if (counted && !edit.value(QStringLiteral("replace_all")).toBool())
        hunk.append(QStringLiteral("@@ -1 +1 @@"));
    if (!removed.isEmpty())
        hunk.append(removed);
    if (!added.isEmpty())
        hunk.append(added);
    return hunk.join(QLatin1Char('\n'));
}

} // namespace

QJsonObject claudeFileChangeItem(const QJsonObject& item) {
    if (item.value(QStringLiteral("type")).toString() != QStringLiteral("dynamicToolCall"))
        return {};
    // A failed edit changed nothing: it stays a tool step with its error, not line counts.
    if (item.value(QStringLiteral("status")).toString() == QStringLiteral("failed") ||
        item.value(QStringLiteral("result")).toObject().value(QStringLiteral("isError")).toBool())
        return {};
    const QString tool = item.value(QStringLiteral("tool")).toString();
    const QJsonObject arguments = toolArguments(item);
    // A filesystem path is kept exactly: it opens, diffs and checks the file.
    QString path = arguments.value(QStringLiteral("file_path")).toString();
    if (path.isEmpty())
        path = arguments.value(QStringLiteral("path")).toString();
    if (path.trimmed().isEmpty())
        return {};
    QJsonObject change{{QStringLiteral("path"), path}};
    if (tool == QStringLiteral("Write")) {
        change.insert(QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), "add"}});
        change.insert(QStringLiteral("diff"),
                      arguments.value(QStringLiteral("content")).toString());
    } else if (tool == QStringLiteral("Edit")) {
        change.insert(QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), "update"}});
        change.insert(QStringLiteral("diff"), replacementHunk(arguments));
    } else if (tool == QStringLiteral("MultiEdit")) {
        // One uncounted replacement leaves the whole file's counts unknown.
        const QJsonArray edits = arguments.value(QStringLiteral("edits")).toArray();
        const bool counted = std::none_of(edits.begin(), edits.end(), [](const QJsonValue& edit) {
            return edit.toObject().value(QStringLiteral("replace_all")).toBool();
        });
        QStringList hunks;
        for (const auto& edit : edits)
            hunks.append(replacementHunk(edit.toObject(), counted));
        change.insert(QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), "update"}});
        change.insert(QStringLiteral("diff"), hunks.join(QLatin1Char('\n')));
    } else {
        return {};
    }
    QJsonObject fileChange{{QStringLiteral("type"), QStringLiteral("fileChange")},
                           {QStringLiteral("id"), item.value(QStringLiteral("id"))},
                           {QStringLiteral("changes"), QJsonArray{change}}};
    if (item.contains(QStringLiteral("status")))
        fileChange.insert(QStringLiteral("status"), item.value(QStringLiteral("status")));
    return fileChange;
}

QString stepSubject(const QString& subtype, const QJsonObject& item) {
    const QJsonObject arguments = toolArguments(item);
    if (subtype == QStringLiteral("skill"))
        return firstString(arguments, {"skill", "name"});
    if (subtype == QStringLiteral("toolsLoaded")) {
        // "select:Monitor,ScheduleWakeup" names the tools it loads.
        QString query = firstString(arguments, {"query"});
        if (query.startsWith(QStringLiteral("select:")))
            query = query.mid(7).split(QLatin1Char(','), Qt::SkipEmptyParts).join(u", ");
        return query;
    }
    if (subtype == QStringLiteral("subAgent"))
        return firstString(arguments, {"description", "subagent_type"});
    if (subtype == QStringLiteral("monitor") || subtype == QStringLiteral("taskStopped"))
        return firstString(arguments, {"description"});
    if (subtype == QStringLiteral("webSearch"))
        return firstString(arguments, {"query"});
    if (subtype == QStringLiteral("webFetch") || subtype == QStringLiteral("webOpen"))
        return pageText(firstString(arguments, {"url", "uri"}));
    if (subtype == QStringLiteral("webFind")) {
        const QString pattern = firstString(arguments, {"pattern", "query"});
        const QString page = pageText(firstString(arguments, {"url", "uri"}));
        if (pattern.isEmpty())
            return page;
        return page.isEmpty() ? QStringLiteral("“%1”").arg(pattern)
                              : QStringLiteral("“%1” on %2").arg(pattern, page);
    }
    return {};
}

QString mcpToolSubject(const QJsonObject& item) {
    const QString tool = item.value(QStringLiteral("tool")).toString();
    QString server = item.value(QStringLiteral("server")).toString();
    QString operation = tool.isEmpty() ? item.value(QStringLiteral("type")).toString() : tool;
    if (server.isEmpty() && tool.startsWith(QStringLiteral("mcp__"))) {
        const QStringList parts = tool.mid(5).split(QStringLiteral("__"));
        if (parts.size() >= 2) {
            server = parts.first();
            operation = parts.mid(1).join(QStringLiteral("__"));
        }
    }
    return server.isEmpty() ? operation : server + QStringLiteral(": ") + operation;
}

CommandStep commandActionStep(const QJsonObject& item) {
    const QJsonArray actions = item.value(QStringLiteral("commandActions")).toArray();
    if (actions.isEmpty())
        return {};
    const QString type = actions.first().toObject().value(QStringLiteral("type")).toString();
    for (const auto& value : actions)
        if (value.toObject().value(QStringLiteral("type")).toString() != type)
            return {};
    // One action names its subject; several of a kind read as the kind alone.
    const QJsonObject action = actions.size() == 1 ? actions.first().toObject() : QJsonObject{};
    const QString path = firstString(action, {"path", "name"});
    const auto titled = [](const QString& done, const QString& doing, const QString& subject) {
        return subject.isEmpty() ? std::pair{done, doing}
                                 : std::pair{done + QStringLiteral(": ") + subject,
                                             doing + QStringLiteral(": ") + subject};
    };
    if (type == QStringLiteral("read")) {
        const auto [done, doing] = titled(QStringLiteral("Read"), QStringLiteral("Reading"), path);
        return {QStringLiteral("fileRead"), done, doing};
    }
    if (type == QStringLiteral("search")) {
        const QString query = firstString(action, {"query"});
        QString subject = query.isEmpty() ? path : QStringLiteral("“%1”").arg(query);
        if (!query.isEmpty() && !path.isEmpty())
            subject += QStringLiteral(" in ") + path;
        const auto [done, doing] =
            titled(QStringLiteral("Searched code"), QStringLiteral("Searching code"), subject);
        return {QStringLiteral("codeSearch"), done, doing};
    }
    if (type == QStringLiteral("listFiles")) {
        const auto [done, doing] =
            titled(QStringLiteral("Listed files"), QStringLiteral("Listing files"), path);
        return {QStringLiteral("fileSearch"), done, doing};
    }
    return {};
}

QString webStepTitle(const QJsonObject& item, const bool completed) {
    const QJsonObject action = item.value(QStringLiteral("action")).toObject();
    const QString type = action.value(QStringLiteral("type")).toString();
    const QString page = pageText(firstString(action, {"url"}));
    QString title;
    QString subject;
    if (type == QStringLiteral("openPage")) {
        title = completed ? QStringLiteral("Opened a page") : QStringLiteral("Opening a page");
        subject = page;
    } else if (type == QStringLiteral("findInPage")) {
        title = completed ? QStringLiteral("Found in page") : QStringLiteral("Finding in page");
        const QString pattern = firstString(action, {"pattern"});
        subject = pattern.isEmpty() ? page
                  : page.isEmpty()  ? QStringLiteral("“%1”").arg(pattern)
                                    : QStringLiteral("“%1” on %2").arg(pattern, page);
    } else {
        title =
            completed ? QStringLiteral("Searched the web") : QStringLiteral("Searching the web");
        subject = firstString(action, {"query"});
        if (subject.isEmpty())
            subject = firstString(item, {"query"});
    }
    return subject.isEmpty() ? title : title + QStringLiteral(": ") + subject;
}

} // namespace ketplus::agent_message
