#include "agent/AgentMessageFormatter.h"
#include "agent/FileChangeStatistics.h"
#include "agent/McpToolResultMedia.h"
#include "agent/TranscriptFileLocation.h"
#include "agent/TranscriptNormalizer.h"
#include "agent/TranscriptStepItems.h"
#include <cmath>

#include "CodexBrowserTools.h"
#include "agent/ConversationTitle.h"
#include "agent/TranscriptPresentationGrouper.h"

#include <QCryptographicHash>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace ketplus::agent_message {

qreal mermaidFrameHeight(const qreal availableWidth) {
    const qreal width = qMax(1.0, availableWidth - 2);
    return qMax(112.0, std::ceil(360.0 / qMax(40.0, width - 64)) * 18 + 56);
}

namespace {

QString categoryForKind(const QString& kind) {
    if (kind == QStringLiteral("user"))
        return QStringLiteral("user");
    if (kind == QStringLiteral("assistant"))
        return QStringLiteral("final");
    if (kind == QStringLiteral("error"))
        return QStringLiteral("error");
    return QStringLiteral("activity");
}

QString eventTypeForKind(const QString& kind) {
    if (kind == QStringLiteral("user"))
        return QStringLiteral("user.message");
    if (kind == QStringLiteral("assistant"))
        return QStringLiteral("assistant.final");
    if (kind == QStringLiteral("error"))
        return QStringLiteral("error.turn");
    return QStringLiteral("activity.generic");
}

QString sanitizedText(QString text) { return TranscriptSanitizer::text(std::move(text)); }

QJsonValue sanitizedJson(const QJsonValue& value, const QString& key = {}) {
    return TranscriptSanitizer::json(value, key);
}

QVariantMap canonical(QVariantMap entry, const QString& eventType, const QString& phase,
                      const QString& title, const QString& summary = {},
                      const QString& target = {}) {
    entry.insert(QStringLiteral("category"), categoryForKind(entry.value("kind").toString()));
    entry.insert(QStringLiteral("eventType"), eventType);
    entry.insert(QStringLiteral("phase"), phase);
    entry.insert(QStringLiteral("title"), title);
    entry.insert(QStringLiteral("summary"), summary);
    entry.insert(QStringLiteral("target"), target);
    entry.insert(QStringLiteral("status"), phase);
    return entry;
}

QString toolTitle(const QJsonObject& item, const bool completed) {
    return QStringLiteral("%1 %2").arg(completed ? QStringLiteral("Used") : QStringLiteral("Using"),
                                       mcpToolSubject(item));
}

QString normalizedToolName(const QJsonObject& item) {
    return item.value(QStringLiteral("tool"))
        .toString()
        .toLower()
        .section(QStringLiteral("__"), -1)
        .section(QLatin1Char('.'), -1);
}

QString semanticToolSubtype(const QJsonObject& item) {
    const QString operation = normalizedToolName(item);
    static const QHash<QString, QString> stepTypes{{"wait", "wait"},
                                                   {"sleep", "wait"},
                                                   {"monitor", "monitor"},
                                                   {"wake", "wake"},
                                                   {"resume_agent", "wake"},
                                                   {"schedulewakeup", "wake"},
                                                   {"skill", "skill"},
                                                   {"load_skill", "skill"},
                                                   {"toolsearch", "toolsLoaded"},
                                                   {"search_tools", "toolsLoaded"},
                                                   {"taskstop", "taskStopped"},
                                                   {"stop_task", "taskStopped"},
                                                   {"agent", "subAgent"},
                                                   {"task", "subAgent"},
                                                   {"spawn_agent", "subAgent"},
                                                   {"update_plan", "plan"},
                                                   {"enterplanmode", "plan"},
                                                   {"webfetch", "webFetch"},
                                                   {"fetch_url", "webFetch"},
                                                   {"websearch", "webSearch"},
                                                   {"find_in_page", "webFind"},
                                                   {"open_page", "webOpen"}};
    if (const auto found = stepTypes.constFind(operation); found != stepTypes.cend())
        return *found;
    if ((operation == QStringLiteral("read_file") || operation == "read"))
        return QStringLiteral("fileRead");
    if ((operation == QStringLiteral("search_files") || operation == "glob"))
        return QStringLiteral("fileSearch");
    if ((operation == QStringLiteral("search_code") || operation == "grep"))
        return QStringLiteral("codeSearch");
    const QString haystack = (item.value(QStringLiteral("server")).toString() + QLatin1Char(' ') +
                              item.value(QStringLiteral("namespace")).toString() +
                              QLatin1Char(' ') + item.value(QStringLiteral("tool")).toString())
                                 .toLower();
    if (haystack.contains(QStringLiteral("view_image")))
        return QStringLiteral("imageView");
    if (haystack.contains(QStringLiteral("browser"))) {
        if (haystack.contains(QStringLiteral("screenshot")))
            return QStringLiteral("browserScreenshot");
        if (haystack.contains(QStringLiteral("navigate")))
            return QStringLiteral("browserNavigate");
        if (haystack.contains(QStringLiteral("click")))
            return QStringLiteral("browserClick");
        if (haystack.contains(QStringLiteral("download")))
            return QStringLiteral("browserDownload");
    }
    if (haystack.contains(QStringLiteral("browser")) ||
        haystack.contains(QStringLiteral("open_url")) ||
        haystack.contains(QStringLiteral("open_website")))
        return QStringLiteral("browserOpen");
    return QStringLiteral("integration");
}

QString firstStringForKeys(const QJsonValue& value, const QSet<QString>& keys) {
    if (value.isObject()) {
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (keys.contains(it.key().toLower()) && it.value().isString() &&
                !it.value().toString().isEmpty())
                return it.value().toString();
        }
        for (auto it = object.begin(); it != object.end(); ++it) {
            const QString found = firstStringForKeys(it.value(), keys);
            if (!found.isEmpty())
                return found;
        }
    } else if (value.isArray()) {
        for (const auto& item : value.toArray()) {
            const QString found = firstStringForKeys(item, keys);
            if (!found.isEmpty())
                return found;
        }
    }
    return {};
}

} // namespace

QString persistedKey(const QString& path) {
    const QByteArray digest =
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("agent/sessions/%1").arg(QString::fromLatin1(digest));
}

QVariantMap message(QString kind, QString body, QString meta, QString filePath, QString itemId) {
    const QString category = categoryForKind(kind);
    const QString eventType = eventTypeForKind(kind);
    const QString state =
        kind == QStringLiteral("error") ? QStringLiteral("failed") : QStringLiteral("completed");
    return {{QStringLiteral("kind"), std::move(kind)},
            {QStringLiteral("body"), std::move(body)},
            {QStringLiteral("meta"), std::move(meta)},
            {QStringLiteral("filePath"), std::move(filePath)},
            {QStringLiteral("itemId"), std::move(itemId)},
            {QStringLiteral("subtype"), QString{}},
            {QStringLiteral("command"), QString{}},
            {QStringLiteral("running"), false},
            {QStringLiteral("category"), category},
            {QStringLiteral("eventType"), eventType},
            {QStringLiteral("phase"), state},
            {QStringLiteral("title"), QString{}},
            {QStringLiteral("summary"), QString{}},
            {QStringLiteral("target"), QString{}},
            {QStringLiteral("status"), state},
            {QStringLiteral("details"), QVariantMap{}},
            {QStringLiteral("sanitizedPayload"), QVariantMap{}}};
}

QString firstLine(const QString& text, const qsizetype maximum) {
    return conversation_title::suggest(text, maximum);
}

QVariantMap fileChangeMessage(const QJsonObject& item, bool completed) {
    const auto changes = item.value(QStringLiteral("changes")).toArray();
    QStringList paths;
    QVariantList statistics;
    for (const auto& value : changes) {
        const auto change = value.toObject();
        const auto path = change.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            continue;
        paths.append(path);
        const auto kind = change.value(QStringLiteral("kind"))
                              .toObject()
                              .value(QStringLiteral("type"))
                              .toString();
        const auto stats = fileChangeStatistics(change);
        const int added = stats.added;
        const int removed = stats.removed;
        const bool known = stats.known;
        const auto changeKind = change.value(QStringLiteral("kind")).toObject();
        const auto destination =
            changeKind.value(QStringLiteral("move_path"))
                .toString(changeKind.value(QStringLiteral("movePath")).toString());
        const QString operation = !destination.isEmpty()             ? QStringLiteral("Moved")
                                  : kind == QStringLiteral("add")    ? QStringLiteral("Created")
                                  : kind == QStringLiteral("delete") ? QStringLiteral("Deleted")
                                                                     : QStringLiteral("Edited");
        statistics.append(
            QVariantMap{{"path", path},
                        {"added", added},
                        {"removed", removed},
                        {"known", known},
                        {"operation", operation},
                        {"destination", destination},
                        {"diffTrimmed",
                         change.value("compacted").toObject().value("policy") == "file-diff-v1"}});
    }
    const QString state =
        completed ? item.value(QStringLiteral("status")).toString() : QStringLiteral("running");
    const QString verb = completed ? QStringLiteral("Edited") : QStringLiteral("Editing");
    QString title = QStringLiteral("%1 %2 %3")
                        .arg(verb)
                        .arg(paths.size())
                        .arg(paths.size() == 1 ? QStringLiteral("file") : QStringLiteral("files"));
    if (item.value(QStringLiteral("compacted")).toObject().value(QStringLiteral("policy")) ==
        QStringLiteral("file-diff-v1"))
        title += QStringLiteral(" · Diff trimmed");
    auto entry = message(QStringLiteral("file"),
                         paths.isEmpty() ? QStringLiteral("File changes") : paths.join('\n'), title,
                         paths.value(0), item.value(QStringLiteral("id")).toString());
    entry.insert(QStringLiteral("subtype"), QStringLiteral("fileChange"));
    entry.insert(QStringLiteral("fileChanges"), statistics);
    entry.insert(QStringLiteral("running"), !completed);
    entry.insert(QStringLiteral("details"), QVariantMap{{QStringLiteral("files"), statistics}});
    entry.insert(QStringLiteral("sanitizedPayload"), sanitizedJson(item).toObject().toVariantMap());
    return canonical(std::move(entry), QStringLiteral("file.change"), state, title,
                     paths.join(QStringLiteral(", ")), paths.value(0));
}

QString selectedSkillInstructions(const QVariantList& skills) {
    QStringList blocks;
    qsizetype totalCharacters = 0;
    constexpr qsizetype maximumSkillCharacters = 16000;
    constexpr qsizetype maximumTotalCharacters = 48000;
    for (const QVariant& value : skills) {
        const QVariantMap skill = value.toMap();
        QString content = skill.value(QStringLiteral("content")).toString().trimmed();
        if (content.isEmpty()) {
            continue;
        }
        if (content.size() > maximumSkillCharacters) {
            content = content.left(maximumSkillCharacters).trimmed() +
                      QStringLiteral("\n\n[Skill truncated by KetPlus]");
        }
        if (totalCharacters + content.size() > maximumTotalCharacters) {
            break;
        }
        totalCharacters += content.size();
        blocks.append(QStringLiteral("## %1\nPath: %2\n\n%3")
                          .arg(skill.value(QStringLiteral("name")).toString(),
                               skill.value(QStringLiteral("path")).toString(), content));
    }
    if (blocks.isEmpty()) {
        return {};
    }
    return QStringLiteral(
               "KetPlus selected skills are enabled for this turn. Follow them when relevant; "
               "the user's direct request still has priority.\n\n%1")
        .arg(blocks.join(QStringLiteral("\n\n---\n\n")));
}

QString errorMessage(const QJsonObject& error) {
    const QString text = error.value(QStringLiteral("message")).toString();
    if (!text.isEmpty())
        return text;
    if (error.isEmpty())
        return {};
    const QString compact =
        QString::fromUtf8(QJsonDocument(error).toJson(QJsonDocument::Compact)).trimmed();
    if (compact == QStringLiteral("{}") || compact == QStringLiteral("null"))
        return {};
    return compact;
}

QString itemSummary(const QJsonObject& item) {
    const QString type = item.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("commandExecution")) {
        return item.value(QStringLiteral("command")).toString();
    }
    if (type == QStringLiteral("mcpToolCall")) {
        return QStringLiteral("%1 · %2").arg(item.value(QStringLiteral("server")).toString(),
                                             item.value(QStringLiteral("tool")).toString());
    }
    if (type == QStringLiteral("dynamicToolCall")) {
        return item.value(QStringLiteral("tool")).toString();
    }
    if (type == QStringLiteral("webSearch")) {
        return item.value(QStringLiteral("query")).toString();
    }
    return type;
}

QString retainedTextSuffix(const QJsonObject& item) {
    const auto info = item.value(QStringLiteral("compacted")).toObject();
    const bool text = info.value(QStringLiteral("policy")) == QStringLiteral("tool-result-text-v1");
    const bool structured =
        info.value(QStringLiteral("structured_result"))
            .toObject()
            .value(QStringLiteral("policy")) == QStringLiteral("tool-structured-result-v1");
    const bool arguments =
        info.value(QStringLiteral("arguments")).toObject().value(QStringLiteral("policy")) ==
        QStringLiteral("tool-arguments-v1");
    if (structured)
        return arguments ? QStringLiteral(" · Result and arguments trimmed")
                         : QStringLiteral(" · Result details trimmed");
    if (arguments)
        return text ? QStringLiteral(" · Text and arguments trimmed")
                    : QStringLiteral(" · Arguments trimmed");
    return text ? QStringLiteral(" · Text trimmed") : QString{};
}

// KetPlus browser API (apps/browser MCP facade, ADR 0008). Inline screenshots go to the
// owner-only transcript media cache; rows keep only file URLs and a sanitized payload.
QVariantMap ketplusBrowserMessage(const QJsonObject& item, const bool completed) {
    const QString tool = item.value(QStringLiteral("tool")).toString();
    const QHash<QString, QString> operations{
        {QStringLiteral("browser_open"), QStringLiteral("open")},
        {QStringLiteral("browser_capture"), QStringLiteral("screenshot")},
        {QStringLiteral("browser_observe"), QStringLiteral("observe")},
        {QStringLiteral("browser_act"), QStringLiteral("act")},
        {QStringLiteral("browser_wait"), QStringLiteral("wait")},
        {QStringLiteral("browser_logs"), QStringLiteral("logs")},
        {QStringLiteral("browser_network"), QStringLiteral("network")},
        {QStringLiteral("browser_inspect"), QStringLiteral("inspect")},
        {QStringLiteral("visual_compare"), QStringLiteral("compare")}};
    const QString operation = operations.value(tool, QStringLiteral("session"));
    QString label = tool;
    if (label.startsWith(QStringLiteral("browser_")))
        label = label.mid(8);
    label.replace(QLatin1Char('_'), QLatin1Char(' '));
    const QJsonObject arguments = item.value(QStringLiteral("arguments")).toObject();
    const QString url = TranscriptSanitizer::url(arguments.value(QStringLiteral("url")).toString());
    const QString summary = mcp_media::argumentSummary(arguments);
    const QJsonValue safePayload = sanitizedJson(item);
    QString title = browserStepTitle(operation, safePayload.toObject(), completed);
    if (title.isEmpty())
        title = QStringLiteral("%1 ketplus_browser · %2")
                    .arg(completed ? QStringLiteral("Used") : QStringLiteral("Using"),
                         sanitizedText(label));
    QVariantList images;
    if (completed)
        images = mcp_media::materializeImages(item.value(QStringLiteral("result")).toObject());
    // The mock's "Captured 2 screenshots: 1440 × 900", read from the first image's header.
    if (operation == QStringLiteral("screenshot") && !images.isEmpty()) {
        if (images.size() > 1)
            title += QStringLiteral(" %1 screenshots").arg(images.size());
        if (QImageReader reader(QUrl(images.first().toString()).toLocalFile());
            reader.size().isValid())
            title +=
                QStringLiteral(": %1 × %2").arg(reader.size().width()).arg(reader.size().height());
    }
    title += retainedTextSuffix(item);
    auto entry = message(QStringLiteral("tool"), summary, QStringLiteral("Browser"), {},
                         item.value(QStringLiteral("id")).toString());
    entry.insert(QStringLiteral("subtype"), QStringLiteral("ketplusBrowser"));
    entry.insert(QStringLiteral("running"), !completed);
    entry.insert(QStringLiteral("details"), safePayload.toObject().toVariantMap());
    entry.insert(QStringLiteral("sanitizedPayload"), safePayload.toObject().toVariantMap());
    if (!images.isEmpty())
        entry.insert(QStringLiteral("images"), images);
    const bool failed =
        item.value(QStringLiteral("status")).toString() == QStringLiteral("failed") ||
        item.value(QStringLiteral("result")).toObject().value(QStringLiteral("isError")).toBool();
    return canonical(std::move(entry), QStringLiteral("browser.") + operation,
                     completed ? (failed ? QStringLiteral("failed") : QStringLiteral("completed"))
                               : QStringLiteral("running"),
                     title, summary, url);
}

QVariantMap webSearchMessage(const QJsonObject& item, bool completed) {
    const QString query = sanitizedText(itemSummary(item));
    const QString title = webStepTitle(sanitizedJson(item).toObject(), completed);
    auto entry = message(QStringLiteral("tool"), query,
                         completed ? QStringLiteral("Search web · completed")
                                   : QStringLiteral("Search web · running"),
                         {}, item.value(QStringLiteral("id")).toString());
    entry.insert(QStringLiteral("subtype"), QStringLiteral("webSearch"));
    entry.insert(QStringLiteral("running"), !completed);
    entry.insert(QStringLiteral("details"), QVariantMap{{QStringLiteral("query"), query}});
    entry.insert(QStringLiteral("sanitizedPayload"), sanitizedJson(item).toObject().toVariantMap());
    return canonical(std::move(entry), QStringLiteral("search.web"),
                     completed ? QStringLiteral("completed") : QStringLiteral("running"), title,
                     query, query);
}

QString commandSummary(const QString& command) {
    QString compact = command.simplified();
    constexpr qsizetype maximumCommandCharacters = 180;
    if (compact.size() > maximumCommandCharacters) {
        compact = compact.left(maximumCommandCharacters - 1).trimmed() + QString::fromUtf8("…");
    }
    return compact;
}

QString boundedCommandOutput(QString output) {
    constexpr qsizetype maximumCharacters = 128 * 1024;
    constexpr auto truncated = "[Earlier command output truncated]\n";
    if (output.size() <= maximumCharacters)
        return output;
    return QString::fromLatin1(truncated) + output.right(maximumCharacters - qstrlen(truncated));
}

// Keep the exact shell command for copy/export and expanded receipts, while making the common
// read/search wrappers legible in the compact activity timeline.
struct CommandPresentation final {
    QString subtype{QStringLiteral("command")};
    QString completedTitle;
    QString runningTitle;
    // The file a read names, as written in the command, with the first line it shows.
    QString readPath;
    int readLine{0};
};

QString concisePath(QString value) {
    value = pathOperand(std::move(value));
    const qsizetype slash = value.lastIndexOf(u'/');
    return slash >= 0 ? value.mid(slash + 1) : value;
}

CommandPresentation commandPresentation(const QString& command) {
    static const QRegularExpression build(
        R"(\bcmake\s+--build\s+\S+(?:\s+--target\s+([^\s|;&]+))?)");
    static const QRegularExpression ctest(R"(\bctest\b)");
    static const QRegularExpression currentBranch(R"(\bgit\s+branch\s+--show-current\b)");
    static const QRegularExpression gitStatus(R"(\bgit\s+status\b)");
    static const QRegularExpression gitAdd(R"(\bgit\s+add\b)");
    if (const auto match = build.match(command); match.hasMatch()) {
        const QString target = concisePath(match.captured(1));
        const QString title = target.isEmpty() ? QStringLiteral("Run build")
                                               : QStringLiteral("Run build: %1").arg(target);
        return {QStringLiteral("command"), title, QStringLiteral("Running build")};
    }
    if (command.contains(ctest))
        return {QStringLiteral("command"), QStringLiteral("Run tests"),
                QStringLiteral("Running tests")};
    if (command.contains(currentBranch))
        return {QStringLiteral("command"), QStringLiteral("Check current branch"),
                QStringLiteral("Checking current branch")};
    if (command.contains(gitStatus))
        return {QStringLiteral("command"), QStringLiteral("Check Git status"),
                QStringLiteral("Checking Git status")};
    if (command.contains(gitAdd))
        return {QStringLiteral("command"), QStringLiteral("Stage files"),
                QStringLiteral("Staging files")};
    // An operand, never an option or a redirection: a trailing `| head -20` names
    // no file, and reading its flag as one used to title the row "Read -20".
    static const QString operand =
        R"(((?:"[^"]+")|(?:'[^']+')|[^-\s|;&<>][^\s|;&<>]*)(?=\s|$|[|;&]))";
    static const QString options = R"((?:\s+-[^\s|;&]*(?:\s+\d+)?)*)";
    static const QRegularExpression sed(R"(\bsed\s+-n\s+['"]?(\d+)(?:,(\d+))?p['"]?\s+)" + operand);
    if (const auto match = sed.match(command); match.hasMatch()) {
        const QString path = concisePath(match.captured(3));
        if (!path.isEmpty()) {
            const QString range =
                match.captured(2).isEmpty()
                    ? QStringLiteral("L%1").arg(match.captured(1))
                    : QStringLiteral("L%1–%2").arg(match.captured(1), match.captured(2));
            const QString title = QStringLiteral("Read: %1 %2").arg(path, range);
            return {QStringLiteral("fileRead"), title, QStringLiteral("Reading %1").arg(path),
                    pathOperand(match.captured(3)), match.captured(1).toInt()};
        }
    }

    static const QRegularExpression reader(R"(\b(?:cat|head|tail)\b)" + options + R"(\s+)" +
                                           operand);
    if (const auto match = reader.match(command); match.hasMatch()) {
        const QString path = concisePath(match.captured(1));
        if (!path.isEmpty()) {
            const QString title = QStringLiteral("Read: %1").arg(path);
            return {QStringLiteral("fileRead"), title, QStringLiteral("Reading %1").arg(path),
                    pathOperand(match.captured(1))};
        }
    }

    static const QRegularExpression search(R"(\b(?:rg|grep)\b[^'"]*['"]([^'"]+)['"])" + options +
                                           R"(\s+)" + operand);
    if (const auto match = search.match(command); match.hasMatch()) {
        const QString query = match.captured(1).simplified().left(48);
        const QString path = concisePath(match.captured(2));
        if (!query.isEmpty() && !path.isEmpty()) {
            const QString title = QStringLiteral("Searched code: “%1” in %2").arg(query, path);
            return {QStringLiteral("codeSearch"), title, QStringLiteral("Searching %1").arg(path)};
        }
    }
    return {};
}

QVariantMap commandMessage(const QString& command, const QString& state, const QString& itemId) {
    const QString safeCommand = sanitizedText(command);
    const bool running =
        state == QStringLiteral("running") || state == QStringLiteral("inProgress");
    const CommandPresentation presentation = commandPresentation(safeCommand);
    const QString title =
        !presentation.completedTitle.isEmpty()
            ? (running ? presentation.runningTitle : presentation.completedTitle)
            : QStringLiteral("%1: %2").arg(
                  running ? QStringLiteral("Running") : QStringLiteral("Ran"), safeCommand);
    QVariantMap entry = message(QStringLiteral("tool"), commandSummary(safeCommand),
                                QStringLiteral("Command · %1").arg(state), {}, itemId);
    entry.insert(QStringLiteral("subtype"), presentation.subtype);
    entry.insert(QStringLiteral("command"), safeCommand);
    entry.insert(QStringLiteral("fileLocation"),
                 fileLocation(presentation.readPath, presentation.readLine, safeCommand, {}));
    entry.insert(QStringLiteral("running"), running);
    return canonical(std::move(entry), QStringLiteral("command.run"),
                     running ? QStringLiteral("running") : state, title);
}

QVariantMap commandMessage(const QJsonObject& item, const bool completed) {
    const QString state =
        completed ? item.value(QStringLiteral("status")).toString() : QStringLiteral("running");
    QVariantMap entry = commandMessage(item.value(QStringLiteral("command")).toString(), state,
                                       item.value(QStringLiteral("id")).toString());
    const QString cwd = sanitizedText(item.value(QStringLiteral("cwd")).toString());
    const QString output = boundedCommandOutput(
        sanitizedText(item.value(QStringLiteral("aggregatedOutput")).toString()));
    QVariantMap details{{QStringLiteral("cwd"), cwd},
                        {QStringLiteral("output"), output},
                        {QStringLiteral("status"), state}};
    if (!item.value(QStringLiteral("exitCode")).isNull())
        details.insert(QStringLiteral("exitCode"), item.value(QStringLiteral("exitCode")).toInt());
    if (!item.value(QStringLiteral("durationMs")).isNull())
        details.insert(QStringLiteral("durationMs"),
                       item.value(QStringLiteral("durationMs")).toVariant());
    entry.insert(QStringLiteral("cwd"), cwd);
    if (entry.value(QStringLiteral("fileLocation")).toString().isEmpty()) {
        const CommandPresentation presentation =
            commandPresentation(entry.value(QStringLiteral("command")).toString());
        entry.insert(QStringLiteral("fileLocation"),
                     fileLocation(presentation.readPath, presentation.readLine,
                                  entry.value(QStringLiteral("command")).toString(), cwd));
    }
    // What the agent said the command is for, read before the command itself.
    entry.insert(QStringLiteral("commandDescription"),
                 sanitizedText(item.value(QStringLiteral("description")).toString().simplified()));
    if (item.value(QStringLiteral("commandActions")).isArray()) {
        const QJsonArray actions = item.value(QStringLiteral("commandActions")).toArray();
        details.insert(QStringLiteral("actions"), sanitizedJson(actions).toArray().toVariantList());
        // A listing's folders, for the group line's "listed 1 folder".
        const bool listing =
            !actions.isEmpty() &&
            std::all_of(actions.begin(), actions.end(), [](const QJsonValue& action) {
                return action.toObject().value(QStringLiteral("type")).toString() ==
                       QStringLiteral("listFiles");
            });
        if (listing)
            entry.insert(QStringLiteral("activityFacts"),
                         QVariantMap{{QStringLiteral("listedFolders"), int(actions.size())}});
    }
    entry.insert(QStringLiteral("exitCode"), item.value(QStringLiteral("exitCode")).toVariant());
    entry.insert(QStringLiteral("durationMs"),
                 item.value(QStringLiteral("durationMs")).toVariant());
    entry.insert(QStringLiteral("details"), details);
    entry.insert(QStringLiteral("sanitizedPayload"), sanitizedJson(item).toObject().toVariantMap());
    // Codex says what a command did; its reading wins over parsing the command line.
    // A command line the parser already read (a sed range, say) keeps its finer title.
    if (const auto step = commandActionStep(sanitizedJson(item).toObject());
        !step.subtype.isEmpty() &&
        entry.value(QStringLiteral("subtype")) == QStringLiteral("command")) {
        const bool running = entry.value(QStringLiteral("running")).toBool();
        entry.insert(QStringLiteral("subtype"), step.subtype);
        entry.insert(QStringLiteral("title"), running ? step.runningTitle : step.completedTitle);
    }
    return entry;
}

QVariantMap assistantFinalMessage(const QJsonObject& item, const bool completed) {
    const QString text = item.value(QStringLiteral("text")).toString();
    auto entry = message(QStringLiteral("assistant"), text,
                         completed ? QStringLiteral("Codex") : QStringLiteral("Codex · streaming"),
                         {}, item.value(QStringLiteral("id")).toString());
    entry.insert(QStringLiteral("running"), !completed);
    return canonical(std::move(entry), QStringLiteral("assistant.final"),
                     completed ? QStringLiteral("completed") : QStringLiteral("streaming"),
                     QStringLiteral("Answer"));
}

QVariantMap activityMessage(const QJsonObject& item, const bool completed) {
    const QString type = item.value(QStringLiteral("type")).toString();
    const QString id = item.value(QStringLiteral("id")).toString();
    if (type == QStringLiteral("commandExecution"))
        return commandMessage(item, completed);
    if (type == QStringLiteral("fileChange"))
        return fileChangeMessage(item, completed);
    if (const auto edit = claudeFileChangeItem(item); !edit.isEmpty())
        return fileChangeMessage(edit, completed);
    if (type == QStringLiteral("webSearch"))
        return webSearchMessage(item, completed);
    if (type == QStringLiteral("imageView")) {
        const QString path = item.value(QStringLiteral("path")).toString();
        QVariantList images =
            mcp_media::materializeImages(item.value(QStringLiteral("result")).toObject());
        if (images.isEmpty() && !path.isEmpty())
            images.append(QUrl::fromLocalFile(path).toString());
        const QString title =
            completed ? QStringLiteral("Viewed an image") : QStringLiteral("Viewing an image");
        const QString resource = images.isEmpty() ? QString{} : images.first().toString();
        auto entry = message(QStringLiteral("tool"), path, QStringLiteral("Image"),
                             QUrl(resource).toLocalFile(), id);
        entry.insert(QStringLiteral("subtype"), QStringLiteral("imageView"));
        entry.insert(QStringLiteral("images"), images);
        entry.insert(QStringLiteral("running"), !completed);
        entry.insert(QStringLiteral("details"), QVariantMap{{QStringLiteral("path"), path}});
        entry.insert(QStringLiteral("sanitizedPayload"),
                     sanitizedJson(item).toObject().toVariantMap());
        return canonical(std::move(entry), QStringLiteral("image.view"),
                         completed ? QStringLiteral("completed") : QStringLiteral("running"), title,
                         path, resource);
    }
    if (type == QStringLiteral("imageGeneration")) {
        const QString path = item.value(QStringLiteral("savedPath")).toString();
        QVariantList images =
            mcp_media::materializeImages(item.value(QStringLiteral("result")).toObject());
        if (images.isEmpty() && !path.isEmpty())
            images.append(QUrl::fromLocalFile(path).toString());
        const QString title = completed ? QStringLiteral("Generated an image")
                                        : QStringLiteral("Generating an image");
        const QString resource = images.isEmpty() ? QString{} : images.first().toString();
        // The mock's "1024 × 1024 · transparent", read from the image's header only.
        QString size;
        if (QImageReader reader(QUrl(resource).toLocalFile()); reader.size().isValid()) {
            size = QStringLiteral("%1 × %2").arg(reader.size().width()).arg(reader.size().height());
            if (QImage::toPixelFormat(reader.imageFormat()).alphaUsage() == QPixelFormat::UsesAlpha)
                size += QStringLiteral(" · transparent");
        }
        auto entry = message(QStringLiteral("tool"), path, QStringLiteral("Image generation"),
                             QUrl(resource).toLocalFile(), id);
        entry.insert(QStringLiteral("subtype"), QStringLiteral("imageGeneration"));
        entry.insert(QStringLiteral("images"), images);
        entry.insert(QStringLiteral("running"), !completed);
        entry.insert(QStringLiteral("details"), QVariantMap{{QStringLiteral("path"), path}});
        entry.insert(QStringLiteral("sanitizedPayload"),
                     sanitizedJson(item).toObject().toVariantMap());
        return canonical(std::move(entry), QStringLiteral("image.generate"),
                         completed ? QStringLiteral("completed") : QStringLiteral("running"), title,
                         size.isEmpty() ? path : size, resource);
    }
    if (type == QStringLiteral("mcpToolCall") &&
        item.value(QStringLiteral("server")).toString() == QLatin1String(browser_tools::serverName))
        return ketplusBrowserMessage(item, completed);
    if (type == QStringLiteral("mcpToolCall") || type == QStringLiteral("dynamicToolCall")) {
        const QString subtype = semanticToolSubtype(item);
        const QJsonValue safePayload = sanitizedJson(item);
        QString url =
            firstStringForKeys(safePayload, {QStringLiteral("url"), QStringLiteral("uri")});
        QString title = toolTitle(item, completed);
        QString eventType = type == QStringLiteral("mcpToolCall")
                                ? QStringLiteral("integration.mcp")
                                : QStringLiteral("tool.dynamic");
        QString readLocation;
        static const QHash<QString, QStringList> stepLabels{
            {"wait", {"Waited", "Waiting"}},
            {"monitor", {"Waited", "Waiting"}},
            {"wake", {"Resumed agent", "Resuming agent"}},
            {"skill", {"Used skill", "Using skill"}},
            {"toolsLoaded", {"Loaded tools", "Finding tools"}},
            {"taskStopped", {"Stopped a background task", "Stopping a background task"}},
            {"subAgent", {"Ran a subagent", "Running a subagent"}},
            {"plan", {"Updated plan", "Updating plan"}},
            {"webFetch", {"Fetched", "Fetching"}},
            {"webSearch", {"Searched the web", "Searching the web"}},
            {"webFind", {"Found in page", "Finding in page"}},
            {"webOpen", {"Opened a page", "Opening a page"}}};
        if (const auto labels = stepLabels.constFind(subtype); labels != stepLabels.cend()) {
            title = labels->at(completed ? 0 : 1);
            const auto operation = normalizedToolName(item);
            if (operation == QStringLiteral("schedulewakeup"))
                title = completed ? QStringLiteral("Scheduled wake-up")
                                  : QStringLiteral("Scheduling wake-up");
            else if (operation == QStringLiteral("enterplanmode"))
                title = completed ? QStringLiteral("Entered plan mode")
                                  : QStringLiteral("Entering plan mode");
            else if (const auto subject = stepSubject(subtype, safePayload.toObject());
                     !subject.isEmpty())
                title += QStringLiteral(": ") + subject;
            eventType = QStringLiteral("activity.") + subtype;
        }
        if (subtype.startsWith(QStringLiteral("browser"))) {
            const QHash<QString, QStringList> operations{
                {QStringLiteral("browserOpen"), {"open", "Opened website", "Opening website"}},
                {QStringLiteral("browserNavigate"), {"navigate", "Navigated", "Navigating"}},
                {QStringLiteral("browserClick"), {"click", "Clicked", "Clicking"}},
                {QStringLiteral("browserDownload"), {"download", "Downloaded", "Downloading"}},
                {QStringLiteral("browserScreenshot"),
                 {"screenshot", "Captured screenshot", "Capturing screenshot"}}};
            const auto operation = operations.value(subtype);
            eventType = QStringLiteral("browser.") + operation.at(0);
            title = operation.at(completed ? 1 : 2);
            const auto pageTitle = firstStringForKeys(
                safePayload, {QStringLiteral("pagetitle"), QStringLiteral("title")});
            const auto domain = QUrl(url).host();
            if (!pageTitle.isEmpty())
                title += QStringLiteral(": ") + pageTitle;
            if (!domain.isEmpty())
                title += QStringLiteral(" · ") + domain;
        } else if (subtype == QStringLiteral("fileRead") ||
                   subtype == QStringLiteral("fileSearch") ||
                   subtype == QStringLiteral("codeSearch")) {
            const bool read = subtype == QStringLiteral("fileRead");
            const bool files = subtype == QStringLiteral("fileSearch");
            eventType = read    ? QStringLiteral("file.read")
                        : files ? QStringLiteral("search.files")
                                : QStringLiteral("search.code");
            url = firstStringForKeys(
                safePayload,
                read ? QSet<QString>{QStringLiteral("path"), QStringLiteral("file"),
                                     QStringLiteral("file_path")}
                     : QSet<QString>{QStringLiteral("query"), QStringLiteral("pattern")});
            title = read    ? (completed ? QStringLiteral("Read") : QStringLiteral("Reading"))
                    : files ? (completed ? QStringLiteral("Searched files")
                                         : QStringLiteral("Searching files"))
                            : (completed ? QStringLiteral("Searched code")
                                         : QStringLiteral("Searching code"));
            if (!url.isEmpty())
                title += QStringLiteral(": ") + url;
            if (read) {
                int offset = 0;
                for (const auto* key : {"arguments", "input"})
                    offset = std::max(offset, item.value(QLatin1String(key))
                                                  .toObject()
                                                  .value(QStringLiteral("offset"))
                                                  .toInt());
                readLocation = fileLocation(url, offset, {}, {});
            }
        } else if (subtype == QStringLiteral("imageView")) {
            eventType = QStringLiteral("image.view");
            title =
                completed ? QStringLiteral("Viewed an image") : QStringLiteral("Viewing an image");
        }
        title += retainedTextSuffix(item);
        auto entry = message(QStringLiteral("tool"), url, QStringLiteral("Activity"), {}, id);
        entry.insert(QStringLiteral("subtype"), subtype);
        // The MCP server, for the group line's "Used node_repl, ketplus_agents".
        if (type == QStringLiteral("mcpToolCall"))
            entry.insert(
                QStringLiteral("activityFacts"),
                QVariantMap{{QStringLiteral("server"),
                             sanitizedText(item.value(QStringLiteral("server")).toString())}});
        entry.insert(QStringLiteral("running"), !completed);
        entry.insert(QStringLiteral("fileLocation"), readLocation);
        entry.insert(QStringLiteral("details"), safePayload.toObject().toVariantMap());
        if (subtype == QStringLiteral("imageView") ||
            subtype == QStringLiteral("browserScreenshot")) {
            const auto imagePath = firstStringForKeys(
                safePayload, {QStringLiteral("path"), QStringLiteral("image_path")});
            if (!imagePath.isEmpty()) {
                const QUrl imageUrl(imagePath);
                if (imageUrl.scheme().isEmpty() || imageUrl.isLocalFile())
                    entry.insert(QStringLiteral("images"),
                                 QVariantList{imageUrl.isLocalFile()
                                                  ? imageUrl.toString()
                                                  : QUrl::fromLocalFile(imagePath).toString()});
            }
        }
        const auto resultImages = mcp_media::materializeImages(item.value("result").toObject());
        if (!resultImages.isEmpty())
            entry.insert("images", resultImages);
        entry.insert(QStringLiteral("sanitizedPayload"), safePayload.toObject().toVariantMap());
        const bool failed =
            item.value(QStringLiteral("status")).toString() == QStringLiteral("failed") ||
            item.value(QStringLiteral("result"))
                .toObject()
                .value(QStringLiteral("isError"))
                .toBool();
        return canonical(std::move(entry), eventType,
                         completed
                             ? (failed ? QStringLiteral("failed") : QStringLiteral("completed"))
                             : QStringLiteral("running"),
                         title, itemSummary(item), url);
    }
    if (type == QStringLiteral("collabAgentToolCall") ||
        type == QStringLiteral("subAgentActivity")) {
        const bool collaborationCall = type == QStringLiteral("collabAgentToolCall");
        // Named as the mock's "Ran a subagent", like Cursor's Task tool.
        const QString title = collaborationCall
                                  ? (completed ? QStringLiteral("Used a collaborator")
                                               : QStringLiteral("Using a collaborator"))
                                  : (completed ? QStringLiteral("Ran a subagent")
                                               : QStringLiteral("Running a subagent"));
        auto entry = message(QStringLiteral("tool"), itemSummary(item),
                             QStringLiteral("Collaboration"), {}, id);
        entry.insert(QStringLiteral("subtype"), collaborationCall ? QStringLiteral("collaboration")
                                                                  : QStringLiteral("subAgent"));
        entry.insert(QStringLiteral("running"), !completed);
        // The agents it addressed, so the group line counts each subagent once.
        QVariantList receivers;
        for (const QJsonValue& receiver : item.value(QStringLiteral("receiverThreadIds")).toArray())
            receivers.append(sanitizedText(receiver.toString()));
        entry.insert(QStringLiteral("activityFacts"),
                     QVariantMap{{QStringLiteral("receivers"), receivers}});
        entry.insert(QStringLiteral("details"), sanitizedJson(item).toObject().toVariantMap());
        entry.insert(QStringLiteral("sanitizedPayload"),
                     sanitizedJson(item).toObject().toVariantMap());
        return canonical(std::move(entry),
                         collaborationCall ? QStringLiteral("integration.collaboration")
                                           : QStringLiteral("integration.subagent"),
                         completed ? QStringLiteral("completed") : QStringLiteral("running"), title,
                         type);
    }
    if (type == QStringLiteral("sleep")) {
        const QString title = completed ? QStringLiteral("Waited") : QStringLiteral("Waiting");
        auto entry = message(QStringLiteral("tool"), {}, QStringLiteral("Activity"), {}, id);
        entry.insert(QStringLiteral("subtype"), QStringLiteral("wait"));
        entry.insert(QStringLiteral("running"), !completed);
        entry.insert(QStringLiteral("details"), sanitizedJson(item).toObject().toVariantMap());
        entry.insert(QStringLiteral("sanitizedPayload"),
                     sanitizedJson(item).toObject().toVariantMap());
        return canonical(std::move(entry), QStringLiteral("activity.wait"),
                         completed ? QStringLiteral("completed") : QStringLiteral("running"),
                         title);
    }
    if (type == QStringLiteral("contextCompaction")) {
        const QString title =
            completed ? QStringLiteral("Compacted context") : QStringLiteral("Compacting context");
        auto entry = message(QStringLiteral("tool"),
                             completed ? QStringLiteral("Context compacted")
                                       : QStringLiteral("Compacting context…"),
                             QStringLiteral("Codex"), {}, id);
        entry.insert(QStringLiteral("subtype"), QStringLiteral("compaction"));
        entry.insert(QStringLiteral("running"), !completed);
        entry.insert(QStringLiteral("sanitizedPayload"),
                     sanitizedJson(item).toObject().toVariantMap());
        return canonical(std::move(entry), QStringLiteral("system.compaction"),
                         completed ? QStringLiteral("completed") : QStringLiteral("running"),
                         title);
    }

    const QString safeType = type.isEmpty() ? QStringLiteral("unknown") : type;
    const auto safeItem = sanitizedJson(item).toObject();
    const QString resource = safeItem.value(QStringLiteral("url"))
                                 .toString(safeItem.value(QStringLiteral("path")).toString());
    const QString title = QStringLiteral("%1 %2").arg(
        completed ? QStringLiteral("Used") : QStringLiteral("Using"), safeType);
    auto entry = message(QStringLiteral("tool"), {}, QStringLiteral("Activity"), {}, id);
    entry.insert(QStringLiteral("subtype"), QStringLiteral("generic"));
    entry.insert(QStringLiteral("running"), !completed);
    entry.insert(QStringLiteral("sanitizedPayload"), sanitizedJson(item).toObject().toVariantMap());
    entry.insert(QStringLiteral("details"), sanitizedJson(item).toObject().toVariantMap());
    return canonical(std::move(entry), QStringLiteral("unknown"),
                     completed ? QStringLiteral("completed") : QStringLiteral("running"), title,
                     safeType, resource);
}

bool mergeImageActivity(QVariantMap& existing, const QVariantMap& incoming, const bool completed) {
    if (existing.value(QStringLiteral("eventType")).toString() != QStringLiteral("image.view") ||
        incoming.value(QStringLiteral("eventType")).toString() != QStringLiteral("image.view"))
        return false;

    QVariantMap details = existing.value(QStringLiteral("details")).toMap();
    QStringList itemIds;
    for (const QVariant& value : details.value(QStringLiteral("itemIds")).toList())
        itemIds.append(value.toString());
    const QString existingId = existing.value(QStringLiteral("itemId")).toString();
    if (itemIds.isEmpty() && !existingId.isEmpty())
        itemIds.append(existingId);

    QStringList pendingIds;
    for (const QVariant& value : details.value(QStringLiteral("pendingItemIds")).toList())
        pendingIds.append(value.toString());
    if (details.value(QStringLiteral("itemIds")).toList().isEmpty() &&
        existing.value(QStringLiteral("running")).toBool() && !existingId.isEmpty())
        pendingIds.append(existingId);

    QStringList paths;
    for (const QVariant& value : details.value(QStringLiteral("paths")).toList())
        paths.append(value.toString());
    if (paths.isEmpty()) {
        const QString path = existing.value(QStringLiteral("target")).toString();
        if (!path.isEmpty())
            paths.append(path);
    }

    QVariantList images = existing.value(QStringLiteral("images")).toList();
    const QString incomingId = incoming.value(QStringLiteral("itemId")).toString();
    const bool newItem = incomingId.isEmpty() || !itemIds.contains(incomingId);
    if (newItem) {
        if (!incomingId.isEmpty())
            itemIds.append(incomingId);
        const QVariantList incomingImages = incoming.value(QStringLiteral("images")).toList();
        for (const QVariant& image : incomingImages)
            images.append(image);
        const QString path = incoming.value(QStringLiteral("target")).toString();
        if (!path.isEmpty())
            paths.append(path);
    }

    if (!incomingId.isEmpty()) {
        pendingIds.removeAll(incomingId);
        if (!completed)
            pendingIds.append(incomingId);
    }
    pendingIds.removeDuplicates();
    itemIds.removeDuplicates();
    const int count = std::max(images.size(), itemIds.size());
    const bool running = !pendingIds.isEmpty();
    const QString title =
        count == 1
            ? (running ? QStringLiteral("Viewing an image") : QStringLiteral("Viewed an image"))
            : QStringLiteral("%1 %2 images")
                  .arg(running ? QStringLiteral("Viewing") : QStringLiteral("Viewed"))
                  .arg(count);
    details.insert(QStringLiteral("itemIds"), itemIds);
    details.insert(QStringLiteral("pendingItemIds"), pendingIds);
    details.insert(QStringLiteral("paths"), paths);
    existing.insert(QStringLiteral("images"), images);
    existing.insert(QStringLiteral("body"), paths.join(QLatin1Char('\n')));
    existing.insert(QStringLiteral("summary"), paths.join(QStringLiteral(", ")));
    existing.insert(QStringLiteral("title"), title);
    existing.insert(QStringLiteral("running"), running);
    existing.insert(QStringLiteral("phase"),
                    running ? QStringLiteral("running") : QStringLiteral("completed"));
    existing.insert(QStringLiteral("status"),
                    running ? QStringLiteral("running") : QStringLiteral("completed"));
    existing.insert(QStringLiteral("details"), details);
    return true;
}

// Convert chronological turns once; live turns take precedence over page snapshots.
QVariantList historyMessages(const QJsonArray& turns, QSet<QString>& seenTurns,
                             const QString& sessionId, const quint64 generation) {
    QVariantList canonical;
    qint64 sequence = 0;
    for (const auto& turnValue : turns) {
        const auto turn = turnValue.toObject();
        const auto turnId = turn.value("id").toString();
        if (!turnId.isEmpty() && seenTurns.contains(turnId))
            continue;
        if (!turnId.isEmpty())
            seenTurns.insert(turnId);
        const auto normalized = ActivityNormalizer::normalizeTurn(
            turn, ActivityNormalizer::Context{sessionId, generation, turnId, sequence, 1});
        sequence += turn.value(QStringLiteral("items")).toArray().size();
        for (const auto& event : normalized) {
            if (!event.visible)
                continue;
            canonical.append(event.toVariantMap());
        }
    }
    return TranscriptPresentationGrouper::group(canonical);
}

QString entryCopyText(const QVariantMap& entry) {
    const QString category = entry.value(QStringLiteral("category")).toString();
    const QString kind = entry.value(QStringLiteral("kind")).toString();
    const bool activity = category == QStringLiteral("activity") ||
                          (category.isEmpty() && kind != QStringLiteral("user") &&
                           kind != QStringLiteral("assistant"));
    if (!activity ||
        entry.value(QStringLiteral("subtype")).toString() == QStringLiteral("question"))
        return activity ? transcriptText({entry}) : entry.value(QStringLiteral("body")).toString();
    QVariantMap normalized = entry;
    if (normalized.value(QStringLiteral("category")).toString().isEmpty())
        normalized.insert(QStringLiteral("category"), QStringLiteral("activity"));
    QString text = transcriptText({normalized});
    QStringList outcome;
    const QString status = entry.value(QStringLiteral("status")).toString();
    if (!status.isEmpty())
        outcome.append(status);
    if (const QVariant exitCode = entry.value(QStringLiteral("exitCode"));
        exitCode.isValid() && !exitCode.isNull())
        outcome.append(QStringLiteral("exit code %1").arg(exitCode.toInt()));
    if (!outcome.isEmpty())
        text += (text.isEmpty() ? QString() : QStringLiteral("\n")) +
                outcome.join(QStringLiteral(", "));
    return text;
}

QStringList fencedCodeBlocks(const QString& markdown) {
    QStringList blocks;
    QChar fenceCharacter;
    qsizetype fenceLength = 0;
    QStringList lines;
    for (const QString& line : markdown.split(u'\n')) {
        qsizetype indent = 0;
        while (indent < line.size() && indent < 4 && line.at(indent) == u' ')
            ++indent;
        const QStringView rest = QStringView(line).mid(indent);
        const auto runLength = [&rest](const QChar character) {
            qsizetype length = 0;
            while (length < rest.size() && rest.at(length) == character)
                ++length;
            return length;
        };
        if (fenceLength == 0) {
            if (indent > 3 || rest.isEmpty() || (rest.at(0) != u'`' && rest.at(0) != u'~'))
                continue;
            const qsizetype length = runLength(rest.at(0));
            if (length < 3)
                continue;
            fenceCharacter = rest.at(0);
            fenceLength = length;
            lines.clear();
            continue;
        }
        if (indent <= 3 && !rest.isEmpty() && rest.at(0) == fenceCharacter &&
            runLength(fenceCharacter) >= fenceLength &&
            rest.mid(runLength(fenceCharacter)).trimmed().isEmpty()) {
            blocks.append(lines.join(u'\n'));
            fenceLength = 0;
            continue;
        }
        lines.append(line);
    }
    // An unterminated fence still renders as a code block up to the end of the document.
    if (fenceLength > 0)
        blocks.append(lines.join(u'\n'));
    return blocks;
}

QString qtMarkdownSource(const QString& markdown) {
    if (!markdown.contains(QStringLiteral("```")) && !markdown.contains(QStringLiteral("~~~")))
        return markdown;
    static const QRegularExpression listItem(QStringLiteral(R"(^( *)([-*+]|\d{1,9}[.)])( +|$))"));
    const QStringList lines = markdown.split(u'\n');
    QStringList output;
    qsizetype contentIndent = -1; // content column of the open list item, -1 outside a list
    bool previousBlank = false;
    QChar fenceCharacter;
    qsizetype fenceLength = 0;
    bool fenceInList = false;
    for (qsizetype index = 0; index < lines.size(); ++index) {
        const QString& line = lines.at(index);
        qsizetype indent = 0;
        while (indent < line.size() && line.at(indent) == u' ')
            ++indent;
        const QStringView rest = QStringView(line).mid(indent);
        const auto runLength = [&rest](const QChar character) {
            qsizetype length = 0;
            while (length < rest.size() && rest.at(length) == character)
                ++length;
            return length;
        };
        output.append(line);
        if (fenceLength > 0) {
            if (!rest.isEmpty() && rest.at(0) == fenceCharacter &&
                runLength(fenceCharacter) >= fenceLength &&
                rest.mid(runLength(fenceCharacter)).trimmed().isEmpty()) {
                fenceLength = 0;
                if (fenceInList && index + 1 < lines.size() &&
                    !lines.at(index + 1).trimmed().isEmpty())
                    output.append(QString());
            }
            continue;
        }
        if (rest.trimmed().isEmpty()) {
            previousBlank = true;
            continue;
        }
        const auto item = listItem.match(line);
        if (item.hasMatch())
            contentIndent = item.capturedLength(1) + item.capturedLength(2) +
                            std::max<qsizetype>(1, item.capturedLength(3));
        else if (previousBlank && indent < contentIndent)
            contentIndent = -1;
        previousBlank = false;
        if (item.hasMatch() || (rest.at(0) != u'`' && rest.at(0) != u'~') ||
            runLength(rest.at(0)) < 3)
            continue;
        fenceCharacter = rest.at(0);
        fenceLength = runLength(fenceCharacter);
        fenceInList = contentIndent > 0 && indent >= contentIndent && indent - contentIndent <= 3;
        if (fenceInList && output.size() > 1 && !output.at(output.size() - 2).trimmed().isEmpty())
            output.insert(output.size() - 1, QString());
    }
    return output.join(u'\n');
}

QString transcriptText(const QVariantList& entries) {
    QStringList parts;
    for (const auto& value : entries) {
        const auto entry = value.toMap();
        const auto body = entry.value("body").toString();
        const QString category = entry.value(QStringLiteral("category")).toString();
        if (entry.value(QStringLiteral("subtype")).toString() == QStringLiteral("question")) {
            const auto answers = entry.value(QStringLiteral("questionAnswers")).toMap();
            for (const auto& value : entry.value(QStringLiteral("questions")).toList()) {
                const auto question = value.toMap();
                const auto id = question.value(QStringLiteral("id")).toString();
                parts.append(
                    QStringLiteral("Question: %1\n%2")
                        .arg(sanitizedText(question.value(QStringLiteral("question")).toString()),
                             answers.contains(id)
                                 ? (question.value(QStringLiteral("isSecret")).toBool()
                                        ? QStringLiteral("[redacted]")
                                        : sanitizedText(answers.value(id).toString()))
                             : entry.value(QStringLiteral("running")).toBool()
                                 ? QStringLiteral("Pending")
                                 : QStringLiteral("Closed")));
            }
            continue;
        }
        if (category == QStringLiteral("activity")) {
            if (entry.value(QStringLiteral("kind")).toString() == QStringLiteral("file") &&
                entry.value(QStringLiteral("fileChanges")).toList().isEmpty() && !body.isEmpty()) {
                parts.append(QStringLiteral("Files:\n") + sanitizedText(body));
                continue;
            }
            QString activity = entry.value(QStringLiteral("title")).toString();
            if (activity.isEmpty())
                activity = entry.value(QStringLiteral("meta")).toString();
            if (!activity.isEmpty())
                parts.append(sanitizedText(activity));
            const auto command = entry.value(QStringLiteral("command")).toString();
            if (!command.isEmpty() && !activity.contains(command))
                parts.append(sanitizedText(command));
            const auto target = entry.value(QStringLiteral("target")).toString();
            if (!target.isEmpty() && !activity.contains(target))
                parts.append(sanitizedText(target));
            for (const auto& value : entry.value(QStringLiteral("fileChanges")).toList()) {
                const auto file = value.toMap();
                parts.append(sanitizedText(file.value(QStringLiteral("path")).toString()));
            }
            continue;
        }
        if (body.isEmpty())
            continue;
        parts.append(entry.value("meta", entry.value("kind")).toString() + ":\n" + body);
    }
    return parts.join("\n\n");
}

} // namespace ketplus::agent_message
