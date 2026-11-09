#include "agent/TranscriptStepItems.h"

#include "agent/TranscriptSanitizer.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
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

// A page as the mock writes it: host, port and path, without the scheme or a trailing slash.
QString pageText(const QString& url) {
    const QUrl parsed(url);
    if (!parsed.isValid() || parsed.host().isEmpty())
        return url;
    QString path = parsed.path();
    if (path == QLatin1String("/"))
        path.clear();
    const QString port = parsed.port() > 0 ? QStringLiteral(":%1").arg(parsed.port()) : QString{};
    return parsed.host() + port + path;
}

// A browser tool's own result: the MCP structured content, else the JSON of its text block.
// The text was sanitized as a string; its JSON escapes can spell what that missed, so the
// parsed object is sanitized again before any title reads it.
QJsonObject browserResult(const QJsonObject& item) {
    const QJsonObject result = item.value(QStringLiteral("result")).toObject();
    const QJsonObject structured = result.value(QStringLiteral("structuredContent")).toObject();
    if (!structured.isEmpty())
        return structured;
    const QJsonArray content = result.value(QStringLiteral("content")).toArray();
    if (content.isEmpty())
        return {};
    const QJsonObject block = content.first().toObject();
    if (block.value(QStringLiteral("type")).toString() != QStringLiteral("text"))
        return {};
    return TranscriptSanitizer::json(
               QJsonDocument::fromJson(block.value(QStringLiteral("text")).toString().toUtf8())
                   .object())
        .toObject();
}

struct Verb {
    const char* done;
    const char* doing;
};

QString browserActTitle(const QJsonObject& action, const QJsonObject& result,
                        const bool completed) {
    static const QHash<QString, Verb> verbs{
        {QStringLiteral("click"), {"Clicked", "Clicking"}},
        {QStringLiteral("fill"), {"Filled in", "Filling in"}},
        {QStringLiteral("press"), {"Pressed a key", "Pressing a key"}},
        {QStringLiteral("hover"), {"Hovered", "Hovering"}},
        {QStringLiteral("select"), {"Selected", "Selecting"}},
        {QStringLiteral("scroll"), {"Scrolled", "Scrolling"}},
        {QStringLiteral("navigate"), {"Opened", "Opening"}},
        {QStringLiteral("reload"), {"Reloaded the page", "Reloading the page"}}};
    const QString type = action.value(QStringLiteral("type")).toString();
    Verb verb = verbs.value(type, {"Acted on the page", "Acting on the page"});
    if (type == QStringLiteral("check"))
        verb = action.value(QStringLiteral("checked")).toBool(true)
                   ? Verb{"Checked", "Checking"}
                   : Verb{"Unchecked", "Unchecking"};
    else if (type == QStringLiteral("history"))
        verb = action.value(QStringLiteral("direction")).toString() == QStringLiteral("forward")
                   ? Verb{"Went forward", "Going forward"}
                   : Verb{"Went back", "Going back"};
    // The node the action reached, by its accessible name; never the value typed into it, nor
    // the key pressed, which the sanitizer withholds as it would any field named "key".
    QString subject;
    if (type == QStringLiteral("navigate"))
        subject = pageText(firstString(action, {"url"}));
    else if (const QString name =
                 firstString(result.value(QStringLiteral("target")).toObject(), {"name"});
             !name.isEmpty())
        subject = QStringLiteral("“%1”").arg(name);
    const QString title = QString::fromUtf8(completed ? verb.done : verb.doing);
    return subject.isEmpty() ? title : title + QStringLiteral(": ") + subject;
}

QString browserWaitSubject(const QJsonObject& condition) {
    const QString type = condition.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("text")) {
        const QString text = firstString(condition, {"text"});
        const bool gone =
            condition.value(QStringLiteral("state")).toString() == QStringLiteral("hidden") ||
            condition.value(QStringLiteral("state")).toString() == QStringLiteral("detached");
        return text.isEmpty() ? QString{}
                              : QStringLiteral("text “%1”%2")
                                    .arg(text, gone ? QStringLiteral(" to go") : QString{});
    }
    if (type == QStringLiteral("url")) {
        const QString url = pageText(firstString(condition, {"equals", "startsWith"}));
        return url.isEmpty() ? QStringLiteral("the URL") : QStringLiteral("URL ") + url;
    }
    if (type == QStringLiteral("load"))
        return QStringLiteral("the page to load");
    if (type == QStringLiteral("settled"))
        return QStringLiteral("the page to settle");
    return {};
}

// "0.4% of pixels differ": the share is kept, however small, so a near match never reads as 0.
QString mismatchText(const double ratio) {
    const double percent = ratio * 100.0;
    if (percent <= 0.0)
        return QStringLiteral("no pixels differ");
    if (percent < 0.1)
        return QStringLiteral("under 0.1% of pixels differ");
    return QStringLiteral("%1% of pixels differ").arg(QString::number(percent, 'f', 1));
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

QString browserStepTitle(const QString& operation, const QJsonObject& item, const bool completed) {
    const QJsonObject arguments = toolArguments(item);
    const QJsonObject result = completed ? browserResult(item) : QJsonObject{};
    const auto titled = [completed](const char* done, const char* doing, const QString& subject) {
        const QString title = QString::fromUtf8(completed ? done : doing);
        return subject.isEmpty() ? title : title + QStringLiteral(": ") + subject;
    };
    if (operation == QStringLiteral("open")) {
        const QString page = pageText(firstString(arguments, {"url"}));
        return page.isEmpty() ? titled("Opened a page", "Opening a page", {})
                              : titled("Opened", "Opening", page);
    }
    if (operation == QStringLiteral("act"))
        return browserActTitle(arguments.value(QStringLiteral("action")).toObject(), result,
                               completed);
    if (operation == QStringLiteral("wait"))
        return titled("Waited for", "Waiting for",
                      browserWaitSubject(arguments.value(QStringLiteral("condition")).toObject()));
    if (operation == QStringLiteral("observe")) {
        // The mock's "Storage · 4 meters": the page's title, then how much of it was read.
        QStringList parts;
        if (const QString page =
                firstString(result.value(QStringLiteral("page")).toObject(), {"title"});
            !page.isEmpty())
            parts.append(page);
        if (const QJsonValue nodes = result.value(QStringLiteral("visibleNodeCount"));
            nodes.isDouble())
            parts.append(nodes.toInt() == 1 ? QStringLiteral("1 node")
                                            : QStringLiteral("%1 nodes").arg(nodes.toInt()));
        return titled("Observed the page", "Observing the page", parts.join(QStringLiteral(" · ")));
    }
    if (operation == QStringLiteral("compare")) {
        const QJsonValue ratio = result.value(QStringLiteral("raster"))
                                     .toObject()
                                     .value(QStringLiteral("mismatchRatio"));
        return titled("Compared with the reference", "Comparing with the reference",
                      ratio.isDouble() ? mismatchText(ratio.toDouble()) : QString{});
    }
    static const QHash<QString, Verb> steps{
        {QStringLiteral("screenshot"), {"Captured", "Capturing"}},
        {QStringLiteral("logs"), {"Read the console", "Reading the console"}},
        {QStringLiteral("network"), {"Read the network log", "Reading the network log"}},
        {QStringLiteral("inspect"), {"Inspected the page", "Inspecting the page"}}};
    if (const auto found = steps.constFind(operation); found != steps.cend())
        return titled(found->done, found->doing, {});
    return {};
}

} // namespace ketplus::agent_message
