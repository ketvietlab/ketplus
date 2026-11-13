#include "agent/TranscriptActivityGroups.h"

#include <QSet>
#include <QStringList>

#include <algorithm>

namespace ketplus::agent_message {
namespace {

const QString kGroupKey = QStringLiteral("groupKey");
const QString kGroupRole = QStringLiteral("groupRole");
const QString kGroupSummary = QStringLiteral("groupSummary");
const QString kGroupLatest = QStringLiteral("groupLatest");
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
    static const QStringList names{kGroupKey,  kGroupRole,    kGroupSummary, kGroupLatest,
                                   kGroupSize, kGroupRunning, kGroupFailed};
    return names;
}

bool isGroupableActivity(const QVariantMap& row) {
    // As in the mock, steps that return images (viewed or generated images, browser
    // captures) are steps of their group: an open group shows the image under its step.
    if (row.value(QStringLiteral("presentationHidden")).toBool() ||
        row.value(QStringLiteral("category")).toString() != QStringLiteral("activity") ||
        row.value(QStringLiteral("chunkContinuation")).toBool())
        return false;
    // Rows without a turn (legacy snapshots, local notices) have no reliable boundary.
    if (row.value(QStringLiteral("turnId")).toString().isEmpty())
        return false;
    const QString subtype = row.value(QStringLiteral("subtype")).toString();
    // Questions need an answer, compaction marks a context boundary, and notices are read
    // rather than skimmed; all of them stay visible. Every step collapses, as in the mock:
    // consecutive thoughts, reads, edits, commands, searches, MCP tool calls, image steps and
    // waits between two messages read as one line.
    static const QSet<QString> steps{
        QStringLiteral("fileRead"),      QStringLiteral("fileChange"),
        QStringLiteral("command"),       QStringLiteral("codeSearch"),
        QStringLiteral("fileSearch"),    QStringLiteral("webSearch"),
        QStringLiteral("browser"),       QStringLiteral("ketplusBrowser"),
        QStringLiteral("collaboration"), QStringLiteral("subAgent"),
        QStringLiteral("generic"),       QStringLiteral("integration"),
        QStringLiteral("imageView"),     QStringLiteral("imageGeneration"),
        QStringLiteral("wait"),          QStringLiteral("thought"),
        QStringLiteral("webFetch"),      QStringLiteral("toolsLoaded"),
        QStringLiteral("skill"),         QStringLiteral("taskStopped"),
        QStringLiteral("monitor"),
    };
    // Browser visits, captures and comparisons too.
    return steps.contains(subtype) || subtype.startsWith(QStringLiteral("browser"));
}

void ActivityTally::add(const QString& id, const int amount) {
    for (Family& family : families) {
        if (family.id == id) {
            family.count += amount;
            return;
        }
    }
    families.append({id, amount});
}

void ActivityTally::addServer(const QString& server, const int steps) {
    add(QStringLiteral("server"), steps);
    if (server.isEmpty())
        return;
    for (Family& known : servers) {
        if (known.id == server) {
            known.count += steps;
            return;
        }
    }
    servers.append({server, steps});
}

QString activityGroupLatest(const QVariantMap& row) {
    QString title = row.value(QStringLiteral("title")).toString().trimmed();
    if (title.isEmpty())
        title = row.value(QStringLiteral("summary")).toString().trimmed();
    if (title.isEmpty())
        title = row.value(QStringLiteral("body")).toString().trimmed();
    // One line: the heading elides what is left.
    return title.section(QLatin1Char('\n'), 0, 0).simplified();
}

QString activityGroupSummary(const QList<QVariantMap>& rows, const qsizetype first,
                             const qsizetype count) {
    ActivityTally tally;
    // Spawning, messaging and waiting on one agent are steps on the same subagent.
    QSet<QString> subagents;
    int unnamedSubagents = 0;
    // A total that leaves out a file's unknown counts would read as exact.
    bool statsUnknown = false;
    for (qsizetype index = first; index < first + count && index < rows.size(); ++index) {
        const QVariantMap& row = rows.at(index);
        const QString subtype = row.value(QStringLiteral("subtype")).toString();
        const QVariantMap facts = row.value(QStringLiteral("activityFacts")).toMap();
        if (row.value(QStringLiteral("status")).toString() == QStringLiteral("failed"))
            ++tally.failed;
        if (subtype == QStringLiteral("command")) {
            tally.add(QStringLiteral("command"), 1);
        } else if (subtype == QStringLiteral("fileRead")) {
            tally.add(QStringLiteral("read"), 1);
        } else if (subtype == QStringLiteral("fileChange")) {
            const QVariantList changes = row.value(QStringLiteral("fileChanges")).toList();
            tally.add(QStringLiteral("edit"), changes.isEmpty() ? 1 : int(changes.size()));
            for (const QVariant& value : changes) {
                const QVariantMap change = value.toMap();
                if (!change.value(QStringLiteral("known")).toBool()) {
                    statsUnknown = true;
                    continue;
                }
                tally.statsKnown = true;
                tally.added += change.value(QStringLiteral("added")).toInt();
                tally.removed += change.value(QStringLiteral("removed")).toInt();
            }
        } else if (subtype == QStringLiteral("webSearch")) {
            tally.add(QStringLiteral("search"), 1);
        } else if (subtype == QStringLiteral("codeSearch")) {
            tally.add(QStringLiteral("codeSearch"), 1);
        } else if (subtype == QStringLiteral("fileSearch")) {
            // Codex's listFiles actions read as the mock's "listed 1 folder".
            const int folders = facts.value(QStringLiteral("listedFolders")).toInt();
            if (folders > 0)
                tally.add(QStringLiteral("list"), folders);
            else
                tally.add(QStringLiteral("fileSearch"), 1);
        } else if (subtype.startsWith(QStringLiteral("browser")) ||
                   subtype == QStringLiteral("ketplusBrowser") ||
                   subtype == QStringLiteral("integration")) {
            QString server = facts.value(QStringLiteral("server")).toString();
            // KetPlus's own browser always goes by its server's name, as in the mock.
            if (subtype == QStringLiteral("ketplusBrowser"))
                server = QStringLiteral("ketplus_browser");
            else if (server.isEmpty() && subtype != QStringLiteral("integration"))
                server = QStringLiteral("the browser");
            tally.addServer(server, 1);
        } else if (subtype == QStringLiteral("collaboration") ||
                   subtype == QStringLiteral("subAgent")) {
            tally.add(QStringLiteral("subagent"), 1);
            const QVariantList receivers = facts.value(QStringLiteral("receivers")).toList();
            for (const QVariant& receiver : receivers) {
                if (!receiver.toString().isEmpty())
                    subagents.insert(receiver.toString());
            }
            if (receivers.isEmpty())
                ++unnamedSubagents;
        } else if (subtype == QStringLiteral("wait") || subtype == QStringLiteral("monitor")) {
            tally.add(QStringLiteral("wait"), 1);
        } else if (subtype == QStringLiteral("webFetch") ||
                   subtype == QStringLiteral("toolsLoaded") || subtype == QStringLiteral("skill") ||
                   subtype == QStringLiteral("taskStopped")) {
            tally.add(subtype, 1);
        } else if (subtype == QStringLiteral("thought")) {
            tally.add(QStringLiteral("thought"), 1);
        } else if (subtype == QStringLiteral("imageView")) {
            tally.add(QStringLiteral("image"), 1);
        } else if (subtype == QStringLiteral("imageGeneration")) {
            tally.add(QStringLiteral("generated"), 1);
        } else {
            tally.add(QStringLiteral("tool"), 1);
        }
    }
    tally.subagents = int(subagents.size()) + unnamedSubagents;
    tally.statsKnown = tally.statsKnown && !statsUnknown;
    return activityTallySummary(tally);
}

QString activityTallySummary(const ActivityTally& tally) {
    const auto& families = tally.families;
    // Thoughts are not steps the user counts; a group of thoughts alone still says so.
    const bool onlyThoughts =
        std::all_of(families.cbegin(), families.cend(), [](const ActivityTally::Family& family) {
            return family.id == QStringLiteral("thought");
        });
    QStringList phrases;
    for (const ActivityTally::Family& family : families) {
        if (family.id == QStringLiteral("thought") && !onlyThoughts)
            continue;
        QString phrase;
        if (family.id == QStringLiteral("command")) {
            phrase = QStringLiteral("ran %1").arg(
                counted(family.count, QStringLiteral("command"), QStringLiteral("commands")));
        } else if (family.id == QStringLiteral("read")) {
            // The mock's word for reads: "Explored 3 files".
            phrase =
                QStringLiteral("explored %1")
                    .arg(counted(family.count, QStringLiteral("file"), QStringLiteral("files")));
        } else if (family.id == QStringLiteral("list")) {
            phrase = QStringLiteral("listed %1 folder%2")
                         .arg(family.count)
                         .arg(family.count == 1 ? QString() : QStringLiteral("s"));
        } else if (family.id == QStringLiteral("webFetch")) {
            phrase =
                QStringLiteral("fetched %1")
                    .arg(counted(family.count, QStringLiteral("page"), QStringLiteral("pages")));
        } else if (family.id == QStringLiteral("toolsLoaded")) {
            phrase = QStringLiteral("loaded tools");
        } else if (family.id == QStringLiteral("skill")) {
            phrase = QStringLiteral("used %1").arg(
                counted(family.count, QStringLiteral("skill"), QStringLiteral("skills")));
        } else if (family.id == QStringLiteral("taskStopped")) {
            phrase = QStringLiteral("stopped %1")
                         .arg(counted(family.count, QStringLiteral("background task"),
                                      QStringLiteral("background tasks")));
        } else if (family.id == QStringLiteral("edit")) {
            phrase =
                QStringLiteral("edited %1")
                    .arg(counted(family.count, QStringLiteral("file"), QStringLiteral("files")));
            if (tally.statsKnown)
                phrase += QStringLiteral(" +%1 -%2").arg(tally.added).arg(tally.removed);
        } else if (family.id == QStringLiteral("search")) {
            phrase = family.count == 1
                         ? QStringLiteral("searched the web")
                         : QStringLiteral("searched the web %1 times").arg(family.count);
        } else if (family.id == QStringLiteral("searched")) {
            phrase = family.count == 1 ? QStringLiteral("searched")
                                       : QStringLiteral("searched %1 times").arg(family.count);
        } else if (family.id == QStringLiteral("codeSearch")) {
            phrase = family.count == 1 ? QStringLiteral("searched code")
                                       : QStringLiteral("searched code %1 times").arg(family.count);
        } else if (family.id == QStringLiteral("fileSearch")) {
            phrase = family.count == 1
                         ? QStringLiteral("searched files")
                         : QStringLiteral("searched files %1 times").arg(family.count);
        } else if (family.id == QStringLiteral("server")) {
            // Each server by name with its own count: "used ketplus_browser 4 times".
            QStringList named;
            int namedSteps = 0;
            for (const ActivityTally::Family& server : tally.servers) {
                named.append(
                    server.count == 1
                        ? QStringLiteral("used %1").arg(server.id)
                        : QStringLiteral("used %1 %2 times").arg(server.id).arg(server.count));
                namedSteps += server.count;
            }
            // Steps whose server is unknown keep the plain count.
            if (family.count > namedSteps)
                named.append(QStringLiteral("used %1").arg(counted(
                    family.count - namedSteps, QStringLiteral("tool"), QStringLiteral("tools"))));
            phrase = named.join(QStringLiteral(", "));
        } else if (family.id == QStringLiteral("subagent")) {
            const int count = std::max(1, tally.subagents);
            phrase = QStringLiteral("used %1").arg(
                counted(count, QStringLiteral("subagent"), QStringLiteral("subagents"), "a"));
        } else if (family.id == QStringLiteral("wait")) {
            phrase = QStringLiteral("waited");
        } else if (family.id == QStringLiteral("thought")) {
            phrase = QStringLiteral("thought");
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
    if (tally.failed > 0)
        summary += QStringLiteral(" (%1 failed)").arg(tally.failed);
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
                           const int failed, const QString& latest = {}) {
        QVariantMap& row = rows[index];
        bool rowChanged = setRole(row, kGroupKey, key);
        rowChanged = setRole(row, kGroupRole, role) || rowChanged;
        rowChanged = setRole(row, kGroupSummary, summary) || rowChanged;
        rowChanged = setRole(row, kGroupLatest, latest) || rowChanged;
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
              int(size), running, failed, activityGroupLatest(rows.at(end - 1)));
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
