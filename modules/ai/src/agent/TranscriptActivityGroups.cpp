#include "agent/TranscriptActivityGroups.h"

#include <QSet>
#include <QStringList>

#include <algorithm>

namespace ketplus::agent_message {
namespace {

const QString kGroupKey = QStringLiteral("groupKey");
const QString kGroupRole = QStringLiteral("groupRole");
const QString kGroupSummary = QStringLiteral("groupSummary");
const QString kGroupSize = QStringLiteral("groupSize");
const QString kGroupRunning = QStringLiteral("groupRunning");
const QString kGroupFailed = QStringLiteral("groupFailed");
const QString kGroupLeaderKey = QStringLiteral("groupLeaderKey");

// "a command" / "3 commands"
QString counted(const int count, const QString& singular, const QString& plural,
                const QString& article = QStringLiteral("a")) {
    return count == 1 ? QStringLiteral("%1 %2").arg(article, singular)
                      : QStringLiteral("%1 %2").arg(count).arg(plural);
}

bool setRole(QVariantMap& row, const QString& key, const QVariant& value) {
    if (row.value(key) == value && row.contains(key))
        return false;
    row.insert(key, value);
    return true;
}

} // namespace

const QString& activityGroupLeaderKeyName() { return kGroupLeaderKey; }

const QStringList& activityGroupRoleNames() {
    static const QStringList names{kGroupKey,  kGroupRole,    kGroupSummary,
                                   kGroupSize, kGroupRunning, kGroupFailed};
    return names;
}

bool isGroupableActivity(const QVariantMap& row) {
    // Image evidence must remain visible even after the surrounding activities collapse.
    if (row.value(QStringLiteral("presentationHidden")).toBool() ||
        row.value(QStringLiteral("category")).toString() != QStringLiteral("activity") ||
        row.value(QStringLiteral("chunkContinuation")).toBool() ||
        !row.value(QStringLiteral("images")).toList().isEmpty())
        return false;
    // Rows without a turn (legacy snapshots, local notices) have no reliable boundary.
    if (row.value(QStringLiteral("turnId")).toString().isEmpty())
        return false;
    const QString subtype = row.value(QStringLiteral("subtype")).toString();
    // Capture and visual comparison always keep their own rows, including while
    // running or if their images are unavailable. Other steps group until they return images.
    const QString eventType = row.value(QStringLiteral("eventType")).toString();
    if (eventType == QStringLiteral("browser.screenshot") ||
        eventType == QStringLiteral("browser.compare"))
        return false;
    // Questions need an answer, compaction marks a context boundary, and reasoning and
    // notices are read rather than skimmed; all of them stay visible. Every tool step
    // collapses, as in the mock: consecutive reads, edits, commands, searches, MCP tool
    // calls, image steps and waits between two messages read as one line.
    static const QSet<QString> steps{
        QStringLiteral("fileRead"),        QStringLiteral("fileChange"),
        QStringLiteral("command"),         QStringLiteral("codeSearch"),
        QStringLiteral("fileSearch"),      QStringLiteral("webSearch"),
        QStringLiteral("browser"),         QStringLiteral("ketplusBrowser"),
        QStringLiteral("collaboration"),   QStringLiteral("generic"),
        QStringLiteral("integration"),     QStringLiteral("imageView"),
        QStringLiteral("imageGeneration"), QStringLiteral("wait"),
    };
    // Browser visits too; evidence rows were kept out above.
    return steps.contains(subtype) || (subtype.startsWith(QStringLiteral("browser")) &&
                                       subtype != QStringLiteral("browserScreenshot"));
}

QString activityGroupSummary(const QList<QVariantMap>& rows, const qsizetype first,
                             const qsizetype count) {
    struct Family final {
        QString id;
        int count{0};
    };
    QList<Family> families;
    const auto add = [&families](const QString& id, const int amount) {
        for (Family& family : families) {
            if (family.id == id) {
                family.count += amount;
                return;
            }
        }
        families.append({id, amount});
    };
    // The servers the group's tool calls used, named as the mock's "Used ketplus_browser,
    // node_repl · 5 steps".
    QStringList servers;
    int serverSteps = 0;
    int added = 0;
    int removed = 0;
    bool statsKnown = false;
    int failed = 0;
    for (qsizetype index = first; index < first + count && index < rows.size(); ++index) {
        const QVariantMap& row = rows.at(index);
        const QString subtype = row.value(QStringLiteral("subtype")).toString();
        if (row.value(QStringLiteral("status")).toString() == QStringLiteral("failed"))
            ++failed;
        if (subtype == QStringLiteral("command")) {
            add(QStringLiteral("command"), 1);
        } else if (subtype == QStringLiteral("fileRead")) {
            add(QStringLiteral("read"), 1);
        } else if (subtype == QStringLiteral("fileChange")) {
            const QVariantList changes = row.value(QStringLiteral("fileChanges")).toList();
            add(QStringLiteral("edit"), changes.isEmpty() ? 1 : int(changes.size()));
            for (const QVariant& value : changes) {
                const QVariantMap change = value.toMap();
                if (!change.value(QStringLiteral("known")).toBool())
                    continue;
                statsKnown = true;
                added += change.value(QStringLiteral("added")).toInt();
                removed += change.value(QStringLiteral("removed")).toInt();
            }
        } else if (subtype == QStringLiteral("webSearch")) {
            add(QStringLiteral("search"), 1);
        } else if (subtype == QStringLiteral("codeSearch")) {
            add(QStringLiteral("codeSearch"), 1);
        } else if (subtype == QStringLiteral("fileSearch")) {
            add(QStringLiteral("fileSearch"), 1);
        } else if (subtype.startsWith(QStringLiteral("browser")) ||
                   subtype == QStringLiteral("ketplusBrowser") ||
                   subtype == QStringLiteral("integration")) {
            QString server = row.value(QStringLiteral("server")).toString();
            if (server.isEmpty())
                server = row.value(QStringLiteral("details"))
                             .toMap()
                             .value(QStringLiteral("server"))
                             .toString();
            // KetPlus's own browser always goes by its server's name, as in the mock.
            if (subtype == QStringLiteral("ketplusBrowser"))
                server = QStringLiteral("ketplus_browser");
            else if (server.isEmpty() && subtype != QStringLiteral("integration"))
                server = QStringLiteral("the browser");
            add(QStringLiteral("server"), 1);
            ++serverSteps;
            if (!server.isEmpty() && !servers.contains(server))
                servers.append(server);
        } else if (subtype == QStringLiteral("collaboration")) {
            add(QStringLiteral("subagent"), 1);
        } else if (subtype == QStringLiteral("wait")) {
            add(QStringLiteral("wait"), 1);
        } else if (subtype == QStringLiteral("imageView")) {
            add(QStringLiteral("image"), 1);
        } else if (subtype == QStringLiteral("imageGeneration")) {
            add(QStringLiteral("generated"), 1);
        } else {
            add(QStringLiteral("tool"), 1);
        }
    }
    QStringList phrases;
    for (const Family& family : families) {
        QString phrase;
        if (family.id == QStringLiteral("command")) {
            phrase = QStringLiteral("ran %1").arg(
                counted(family.count, QStringLiteral("command"), QStringLiteral("commands")));
        } else if (family.id == QStringLiteral("read")) {
            phrase = QStringLiteral("read %1").arg(
                counted(family.count, QStringLiteral("file"), QStringLiteral("files")));
        } else if (family.id == QStringLiteral("edit")) {
            phrase =
                QStringLiteral("edited %1")
                    .arg(counted(family.count, QStringLiteral("file"), QStringLiteral("files")));
            if (statsKnown)
                phrase += QStringLiteral(" +%1 -%2").arg(added).arg(removed);
        } else if (family.id == QStringLiteral("search")) {
            phrase = family.count == 1
                         ? QStringLiteral("searched the web")
                         : QStringLiteral("searched the web %1 times").arg(family.count);
        } else if (family.id == QStringLiteral("codeSearch")) {
            phrase = family.count == 1 ? QStringLiteral("searched code")
                                       : QStringLiteral("searched code %1 times").arg(family.count);
        } else if (family.id == QStringLiteral("fileSearch")) {
            phrase = family.count == 1
                         ? QStringLiteral("searched files")
                         : QStringLiteral("searched files %1 times").arg(family.count);
        } else if (family.id == QStringLiteral("server")) {
            phrase = servers.isEmpty()
                         ? QStringLiteral("used %1").arg(counted(
                               family.count, QStringLiteral("tool"), QStringLiteral("tools")))
                         : QStringLiteral("used ") + servers.join(QStringLiteral(", "));
            // Only a group of tool calls alone counts its steps; a mixed line stays a list.
            if (families.size() == 1 && !servers.isEmpty() && serverSteps > servers.size())
                phrase += QStringLiteral(" · %1 steps").arg(serverSteps);
        } else if (family.id == QStringLiteral("subagent")) {
            phrase = QStringLiteral("used %1").arg(counted(family.count, QStringLiteral("subagent"),
                                                           QStringLiteral("subagents"), "a"));
        } else if (family.id == QStringLiteral("wait")) {
            phrase = QStringLiteral("waited");
        } else if (family.id == QStringLiteral("image")) {
            phrase = QStringLiteral("viewed %1")
                         .arg(counted(family.count, QStringLiteral("image"),
                                      QStringLiteral("images"), "an"));
        } else if (family.id == QStringLiteral("generated")) {
            phrase = QStringLiteral("generated %1")
                         .arg(counted(family.count, QStringLiteral("image"),
                                      QStringLiteral("images"), "an"));
        } else {
            phrase = QStringLiteral("used %1").arg(
                counted(family.count, QStringLiteral("tool"), QStringLiteral("tools")));
        }
        phrases.append(phrase);
    }
    QString summary = phrases.join(QStringLiteral(", "));
    if (!summary.isEmpty())
        summary[0] = summary.at(0).toUpper();
    if (failed > 0)
        summary += QStringLiteral(" (%1 failed)").arg(failed);
    return summary;
}

QList<int> assignActivityGroups(QList<QVariantMap>& rows) {
    return assignActivityGroups(rows, 0, int(rows.size()) - 1);
}

QList<int> assignActivityGroups(QList<QVariantMap>& rows, int first, int last) {
    QList<int> changed;
    if (rows.isEmpty())
        return changed;
    // Include the neighbours so a row that stopped touching a run is cleared, then widen to
    // whole groupable stretches: every run inside [first, last] starts and ends there.
    first = std::clamp(first - 1, 0, int(rows.size()) - 1);
    last = std::clamp(last + 1, 0, int(rows.size()) - 1);
    if (first > last)
        return changed;
    while (first > 0 && isGroupableActivity(rows.at(first - 1)))
        --first;
    while (last + 1 < rows.size() && isGroupableActivity(rows.at(last + 1)))
        ++last;
    const auto apply = [&](const int index, const QString& key, const QString& role,
                           const QString& summary, const int size, const bool running,
                           const int failed) {
        QVariantMap& row = rows[index];
        bool rowChanged = setRole(row, kGroupKey, key);
        rowChanged = setRole(row, kGroupRole, role) || rowChanged;
        rowChanged = setRole(row, kGroupSummary, summary) || rowChanged;
        rowChanged = setRole(row, kGroupSize, size) || rowChanged;
        rowChanged = setRole(row, kGroupRunning, running) || rowChanged;
        rowChanged = setRole(row, kGroupFailed, failed) || rowChanged;
        if (rowChanged)
            changed.append(index);
    };
    QSet<QString> usedKeys;
    qsizetype index = first;
    while (index <= last) {
        if (rows.at(index).value("subtype") == "daemonGroup") {
            ++index; // Wire summary owns its roles; never merge it into a local group.
            continue;
        }
        if (!isGroupableActivity(rows.at(index))) {
            apply(int(index), {}, {}, {}, 0, false, 0);
            rows[index].remove(kGroupLeaderKey);
            ++index;
            continue;
        }
        const QVariant turnId = rows.at(index).value(QStringLiteral("turnId"));
        qsizetype end = index + 1;
        while (end < rows.size() && isGroupableActivity(rows.at(end)) &&
               rows.at(end).value(QStringLiteral("turnId")) == turnId)
            ++end;
        const qsizetype size = end - index;
        if (size < 2) {
            apply(int(index), {}, {}, {}, 0, false, 0);
            rows[index].remove(kGroupLeaderKey);
            index = end;
            continue;
        }
        bool running = false;
        int failed = 0;
        for (qsizetype member = index; member < end; ++member) {
            running = running || rows.at(member).value(QStringLiteral("running")).toBool();
            if (rows.at(member).value(QStringLiteral("status")).toString() ==
                QStringLiteral("failed"))
                ++failed;
        }
        // Stable identity: a run keeps the key one of its rows already carried, so prepending
        // older rows or trimming the leader does not change it. A run split in two keeps the
        // key on its first part only.
        QString key;
        for (qsizetype member = index; member < end && key.isEmpty(); ++member) {
            const QString previous = rows.at(member).value(kGroupKey).toString();
            if (!previous.isEmpty() && !usedKeys.contains(previous))
                key = previous;
        }
        if (key.isEmpty())
            key = rows.at(index).value(QStringLiteral("messageKey")).toString();
        usedKeys.insert(key);
        const QString leaderKey = rows.at(index).value(QStringLiteral("messageKey")).toString();
        apply(int(index), key, QStringLiteral("leader"), activityGroupSummary(rows, index, size),
              int(size), running, failed);
        rows[index].insert(kGroupLeaderKey, leaderKey);
        for (qsizetype member = index + 1; member < end; ++member) {
            apply(int(member), key, QStringLiteral("member"), {}, 0, running, 0);
            rows[member].insert(kGroupLeaderKey, leaderKey);
        }
        index = end;
    }
    return changed;
}

} // namespace ketplus::agent_message
