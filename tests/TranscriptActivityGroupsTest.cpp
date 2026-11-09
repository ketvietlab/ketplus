#include "agent/TranscriptActivityGroups.h"
#include "agent/AgentMessageFormatter.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/TranscriptTurnFiles.h"

#include <QtTest>

using namespace ketplus::agent_message;

namespace {

QVariantMap activity(const QString& key, const QString& subtype, const QString& turn = "turn-1",
                     const QString& status = "completed") {
    return {{"messageKey", key},
            {"category", "activity"},
            {"kind", "tool"},
            {"subtype", subtype},
            {"turnId", turn},
            {"status", status},
            {"running", status == "running"}};
}

QVariantMap text(const QString& key, const QString& category, const QString& turn = "turn-1") {
    return {{"messageKey", key}, {"category", category}, {"turnId", turn}};
}

QVariantMap fileChange(const QString& key, int added, int removed) {
    QVariantMap row = activity(key, "fileChange");
    row.insert("fileChanges",
               QVariantList{QVariantMap{
                   {"path", "a.cpp"}, {"added", added}, {"removed", removed}, {"known", true}}});
    return row;
}

} // namespace

class TranscriptActivityGroupsTest final : public QObject {
    Q_OBJECT

  private slots:
    // The turn's changed files: its file-change rows and its daemon groups' files, merged by
    // path in the order first changed, never another turn's and never a failed change.
    void turnFilesGatherOnlyThisTurnsChanges() {
        using namespace ketplus::agent_message;
        const auto stats = [](const QString& path, int added, int removed,
                              const QString& operation = "Edited", bool known = true) {
            return QVariantMap{{"path", path},   {"destination", ""},  {"operation", operation},
                               {"added", added}, {"removed", removed}, {"known", known}};
        };
        const QList<QVariantMap> rows{
            {{"kind", "user"}, {"turnId", "t1"}},
            {{"subtype", "fileChange"},
             {"turnId", "t1"},
             {"fileChanges", QVariantList{stats("old.cpp", 1, 0)}}},
            {{"subtype", "turnEnd"}, {"turnId", "t1"}},
            {{"kind", "user"}, {"turnId", "t2"}},
            {{"subtype", "fileChange"},
             {"turnId", "t2"},
             {"fileChanges", QVariantList{stats("src/a.cpp", 1, 0)}}},
            {{"subtype", "daemonGroup"},
             {"turnId", "t2"},
             {"groupFiles", QVariantList{stats("src/a.cpp", 2, 1, "Edited", false),
                                         stats("src/b.cpp", 4, 0, "Created")}}},
            {{"subtype", "fileChange"},
             {"turnId", "t2"},
             {"status", "failed"},
             {"fileChanges", QVariantList{stats("src/c.cpp", 9, 9)}}},
            {{"subtype", "turnEnd"}, {"turnId", "t2"}}};
        const QVariantList files = turnChangedFiles(rows, rows.size() - 1);
        QCOMPARE(files.size(), 2);
        QCOMPARE(files.at(0).toMap(), stats("src/a.cpp", 3, 1, "Edited", false));
        QCOMPARE(files.at(1).toMap(), stats("src/b.cpp", 4, 0, "Created"));
        QCOMPARE(turnChangedFiles(rows, 2).size(), 1);
        QVERIFY(turnChangedFiles(rows, 99).isEmpty());
        // ADR 0022: a daemon group row carries its summary's files.
        const auto group = daemonActivityGroupMessage(
            {{"item_id", "activity-group:7"},
             {"group_id", "7"},
             {"count", 1},
             {"families", QJsonArray{QJsonObject{{"family", "edit"}, {"count", 1}}}},
             {"files", QJsonArray{QJsonObject{{"path", "src/a.cpp"}, {"added", 2}}}}},
            3);
        QCOMPARE(group.value("groupFiles").toList().size(), 1);
        QCOMPARE(group.value("groupFiles").toList().first().toMap().value("added").toInt(), 2);
        QCOMPARE(group.value("groupFilesTotal").toInt(), 1);
        // A steer stays in the turn it steers: the changes before and after it both count.
        const QList<QVariantMap> steered{
            {{"kind", "user"}, {"turnId", "t3"}},
            {{"subtype", "fileChange"},
             {"turnId", "t3"},
             {"fileChanges", QVariantList{stats("before.cpp", 1, 0)}}},
            {{"kind", "user"}, {"eventType", "turn.steered"}, {"turnId", "t3"}},
            {{"subtype", "fileChange"},
             {"turnId", "t3"},
             {"fileChanges", QVariantList{stats("after.cpp", 2, 0)}}},
            {{"subtype", "turnEnd"}, {"turnId", "t3"}}};
        bool complete = false;
        const QVariantList both = turnChangedFiles(steered, steered.size() - 1, &complete);
        QCOMPARE(both.size(), 2);
        QCOMPARE(both.at(0).toMap().value("path").toString(), QStringLiteral("before.cpp"));
        QVERIFY(complete);
        // A group that listed only some of its files leaves the turn's list incomplete.
        const auto bounded = daemonActivityGroupMessage(
            {{"item_id", "activity-group:8"},
             {"group_id", "8"},
             {"count", 1},
             {"families", QJsonArray{QJsonObject{{"family", "edit"}, {"count", 60}}}},
             {"files", QJsonArray{QJsonObject{{"path", "gen/0.cpp"}, {"added", 1}}}},
             {"files_total", 60}},
            3);
        QCOMPARE(bounded.value("groupFilesTotal").toInt(), 60);
        auto truncated = steered;
        truncated.insert(4, {{"subtype", "daemonGroup"},
                             {"turnId", "t3"},
                             {"groupFiles", bounded.value("groupFiles")},
                             {"groupFilesTotal", 60}});
        QCOMPARE(turnChangedFiles(truncated, truncated.size() - 1, &complete).size(), 3);
        QVERIFY(!complete);
    }
    void groupsConsecutiveActivityWithinATurn() {
        QList<QVariantMap> rows{text("user", "user"),       activity("c1", "fileRead"),
                                activity("r1", "fileRead"), activity("c2", "fileRead"),
                                fileChange("e1", 1, 1),     activity("r2", "fileRead"),
                                activity("c3", "fileRead"), text("answer", "final")};
        assignActivityGroups(rows);

        // Steps of different kinds between two messages read as one line, as on the phone.
        QCOMPARE(rows.at(0).value("groupRole").toString(), QString());
        QCOMPARE(rows.at(1).value("groupRole").toString(), QStringLiteral("leader"));
        QCOMPARE(rows.at(1).value("groupSize").toInt(), 6);
        QCOMPARE(rows.at(1).value("groupSummary").toString(),
                 QStringLiteral("Read 5 files, edited a file +1 -1"));
        QCOMPARE(rows.at(4).value("groupRole").toString(), QStringLiteral("member"));
        QCOMPARE(rows.at(6).value("groupRole").toString(), QStringLiteral("member"));
        QCOMPARE(rows.at(7).value("groupRole").toString(), QString());
    }

    void boundariesSplitGroups() {
        QList<QVariantMap> rows{
            activity("a1", "fileRead"),           activity("a2", "fileRead"),
            text("commentary", "commentary"),     activity("b1", "fileRead"),
            activity("b2", "fileRead", "turn-2"), activity("b3", "fileRead", "turn-2"),
            activity("q", "question", "turn-2"),  activity("c1", "fileRead", "turn-2")};
        assignActivityGroups(rows);

        QCOMPARE(rows.at(0).value("groupSize").toInt(), 2);
        // A single activity row between boundaries stays ungrouped.
        QCOMPARE(rows.at(3).value("groupRole").toString(), QString());
        QCOMPARE(rows.at(4).value("groupRole").toString(), QStringLiteral("leader"));
        QCOMPARE(rows.at(4).value("groupSummary").toString(), QStringLiteral("Read 2 files"));
        // Questions stay visible and are never folded into a group.
        QCOMPARE(rows.at(6).value("groupRole").toString(), QString());
        QCOMPARE(rows.at(7).value("groupRole").toString(), QString());
    }

    void approvalsStayOutsideGroups() {
        // A permission request lands between the commands of a running turn.
        QList<QVariantMap> rows{activity("c1", "fileRead"), activity("c2", "fileRead"),
                                activity("p", "approval", "turn-1", "running"),
                                activity("c3", "fileRead"), activity("c4", "fileRead")};
        assignActivityGroups(rows);

        QCOMPARE(rows.at(2).value("groupRole").toString(), QString());
        QCOMPARE(rows.at(2).value("groupKey").toString(), QString());
        // It splits the commands instead of joining either side.
        QCOMPARE(rows.at(0).value("groupSize").toInt(), 2);
        QCOMPARE(rows.at(3).value("groupRole").toString(), QStringLiteral("leader"));
        QCOMPARE(rows.at(3).value("groupSize").toInt(), 2);
    }

    void summaryReportsFailuresAndRunningState() {
        QList<QVariantMap> rows{activity("c1", "fileRead", "turn-1", "failed"),
                                activity("c2", "fileRead"), activity("m1", "fileRead"),
                                activity("i1", "fileRead"),
                                activity("c3", "fileRead", "turn-1", "running")};
        assignActivityGroups(rows);

        QCOMPARE(rows.at(0).value("groupSummary").toString(),
                 QStringLiteral("Read 5 files (1 failed)"));
        QCOMPARE(rows.at(0).value("groupFailed").toInt(), 1);
        QVERIFY(rows.at(0).value("groupRunning").toBool());
    }

    void recomputingUnchangedRowsReportsNoChanges() {
        QList<QVariantMap> rows{activity("c1", "fileRead"), activity("c2", "fileRead")};
        QVERIFY(!assignActivityGroups(rows).isEmpty());
        QVERIFY(assignActivityGroups(rows).isEmpty());

        rows.append(activity("c3", "fileRead"));
        const QList<int> changed = assignActivityGroups(rows);
        // The leader's summary and the new member change; the existing member does not.
        QCOMPARE(changed, QList<int>({0, 2}));
        QCOMPARE(rows.at(0).value("groupSummary").toString(), QStringLiteral("Read 3 files"));
    }

    void rangedRecomputeTouchesOnlyTheAffectedRun() {
        QList<QVariantMap> rows{activity("a1", "fileRead"), activity("a2", "fileRead"),
                                text("answer", "final"), activity("b1", "fileRead"),
                                activity("b2", "fileRead", "turn-1", "running")};
        assignActivityGroups(rows);
        QVERIFY(rows.at(4).value("groupRunning").toBool());
        QVERIFY(!rows.at(0).value("groupRunning").toBool());

        rows[4].insert("status", "completed");
        rows[4].insert("running", false);
        // Only the second run (leader + member) changes; the first run is outside the range.
        QCOMPARE(assignActivityGroups(rows, 4, 4), QList<int>({3, 4}));
        QVERIFY(!rows.at(3).value("groupRunning").toBool());

        rows.removeLast();
        QCOMPARE(assignActivityGroups(rows, 3, 3), QList<int>({3}));
        QCOMPARE(rows.at(3).value("groupRole").toString(), QString());
    }

    void groupKeySurvivesLeaderChanges() {
        QList<QVariantMap> rows{activity("c2", "fileRead"), activity("c3", "fileRead"),
                                activity("c4", "fileRead")};
        assignActivityGroups(rows);
        QCOMPARE(rows.at(0).value("groupKey").toString(), QStringLiteral("c2"));

        // Prepending older same-turn activity changes the leader, not the identity.
        rows.prepend(activity("c1", "fileRead"));
        assignActivityGroups(rows, 0, 1);
        QCOMPARE(rows.at(0).value("groupRole").toString(), QStringLiteral("leader"));
        for (const QVariantMap& row : rows)
            QCOMPARE(row.value("groupKey").toString(), QStringLiteral("c2"));
        QCOMPARE(rows.at(3).value("groupLeaderKey").toString(), QStringLiteral("c1"));

        // Trimming the leader (and the original first row) from the front keeps it too.
        rows.removeFirst();
        rows.removeFirst();
        assignActivityGroups(rows, 0, 0);
        QCOMPARE(rows.at(0).value("groupRole").toString(), QStringLiteral("leader"));
        QCOMPARE(rows.at(0).value("groupKey").toString(), QStringLiteral("c2"));
        QCOMPARE(rows.at(1).value("groupLeaderKey").toString(), QStringLiteral("c3"));

        // Back trims keep it; a run reduced to one row is ungrouped.
        rows.append(activity("c5", "fileRead"));
        assignActivityGroups(rows, 2, 2);
        rows.removeLast();
        assignActivityGroups(rows, 1, 1);
        QCOMPARE(rows.at(1).value("groupKey").toString(), QStringLiteral("c2"));
        rows.removeLast();
        assignActivityGroups(rows, 0, 0);
        QCOMPARE(rows.at(0).value("groupKey").toString(), QString());
        QVERIFY(!rows.at(0).contains("groupLeaderKey"));
    }

    void splitRunKeepsKeyOnFirstPart() {
        QList<QVariantMap> rows{activity("a1", "fileRead"), activity("a2", "fileRead"),
                                activity("a3", "fileRead"), activity("a4", "fileRead"),
                                activity("a5", "fileRead")};
        assignActivityGroups(rows);
        rows[2] = text("a3", "commentary");
        assignActivityGroups(rows, 2, 2);
        QCOMPARE(rows.at(0).value("groupKey").toString(), QStringLiteral("a1"));
        // The second part cannot share the identity (its expansion state is separate).
        QCOMPARE(rows.at(3).value("groupKey").toString(), QStringLiteral("a4"));
        QCOMPARE(rows.at(4).value("groupKey").toString(), QStringLiteral("a4"));
    }

    void commandsSearchesAndToolCallsCollapseWithFileSteps() {
        QList<QVariantMap> rows{activity("c1", "command"),        activity("c2", "command"),
                                activity("b1", "ketplusBrowser"), activity("s1", "webSearch"),
                                activity("r1", "fileRead"),       activity("g1", "generic")};
        assignActivityGroups(rows);
        QCOMPARE(rows.at(0).value("groupRole").toString(), QStringLiteral("leader"));
        QCOMPARE(rows.at(0).value("groupSize").toInt(), 6);
        QCOMPARE(rows.at(0).value("groupSummary").toString(),
                 QStringLiteral("Ran 2 commands, used ketplus_browser, searched the web, read a "
                                "file, used a tool"));
    }

    // As in the mock, MCP tool calls, image steps, waits and subagents stay in the group, and
    // its line names the servers used: "Used ketplus_browser, node_repl · 5 steps".
    void toolServersImagesAndWaitsCollapseIntoTheGroup() {
        const auto served = [](const QString& key, const QString& subtype, const QString& server) {
            auto row = activity(key, subtype);
            row.insert("details", QVariantMap{{"server", server}});
            return row;
        };
        QList<QVariantMap> rows{served("b1", "ketplusBrowser", "ketplus_browser"),
                                served("b2", "ketplusBrowser", "ketplus_browser"),
                                served("m1", "integration", "node_repl"),
                                served("m2", "integration", "node_repl"),
                                activity("w1", "webSearch"),
                                activity("v1", "imageView"),
                                activity("i1", "imageGeneration"),
                                activity("z1", "wait"),
                                activity("a1", "collaboration")};
        assignActivityGroups(rows);
        QCOMPARE(rows.at(0).value("groupRole").toString(), QStringLiteral("leader"));
        QCOMPARE(rows.at(0).value("groupSize").toInt(), 9);
        QCOMPARE(rows.at(0).value("groupSummary").toString(),
                 QStringLiteral("Used ketplus_browser, node_repl, searched the web, "
                                "viewed an image, generated an image, waited, used a subagent"));
    }

    void evidenceAndUnknownRowsRemainIndividuallyVisible() {
        QVariantMap capture = activity("b1", "ketplusBrowser");
        capture.insert("eventType", "browser.screenshot");
        QList<QVariantMap> rows{activity("c1", "command"), capture,
                                activity("c2", "command"), activity("u1", "futureTool"),
                                activity("c3", "command"), activity("q1", "question"),
                                activity("c4", "command")};
        assignActivityGroups(rows);
        for (const auto& row : rows)
            QVERIFY(row.value("groupRole").toString().isEmpty());
    }

    void browserStepsWithoutImagesCollapse_data() {
        QTest::addColumn<QString>("tool");
        for (const char* tool : {"browser_open", "browser_observe", "browser_act", "browser_wait",
                                 "browser_logs", "browser_network", "browser_inspect",
                                 "browser_create_session", "browser_close_session"})
            QTest::newRow(tool) << QString::fromLatin1(tool);
    }

    void browserStepsWithoutImagesCollapse() {
        QFETCH(QString, tool);
        const QJsonObject item{
            {"id", "step"}, {"type", "mcpToolCall"}, {"server", "ketplus_browser"}, {"tool", tool}};
        for (const bool completed : {false, true}) {
            auto step = activityMessage(item, completed);
            step.insert("turnId", "turn-1");
            QList<QVariantMap> rows{activity("before", "ketplusBrowser"), step,
                                    activity("after", "ketplusBrowser")};
            assignActivityGroups(rows);
            QCOMPARE(rows.at(0).value("groupSummary").toString(), "Used ketplus_browser · 3 steps");
            QCOMPARE(rows.at(0).value("groupSize").toInt(), 3);
            QCOMPARE(rows.at(1).value("groupRole").toString(), "member");
            QCOMPARE(rows.at(0).value("groupRunning").toBool(), !completed);
        }
    }

    void captureAndComparisonAlwaysSplitBrowserGroups_data() {
        QTest::addColumn<QString>("tool");
        QTest::newRow("capture") << QStringLiteral("browser_capture");
        QTest::newRow("compare") << QStringLiteral("visual_compare");
    }

    void captureAndComparisonAlwaysSplitBrowserGroups() {
        QFETCH(QString, tool);
        auto capture = activityMessage({{"id", "capture"},
                                        {"type", "mcpToolCall"},
                                        {"server", "ketplus_browser"},
                                        {"tool", tool}},
                                       false);
        capture.insert("turnId", "turn-1");
        for (const char* event : {"browser.screenshot", "browser.observe", "browser.compare"}) {
            auto evidence = activity("evidence", "ketplusBrowser");
            evidence.insert("eventType", event);
            evidence.insert("images", QVariantList{"ketplus-media-unavailable:0"});
            QList<QVariantMap> rows{activity("before-1", "ketplusBrowser"),
                                    activity("before-2", "ketplusBrowser"),
                                    capture,
                                    activity("middle-1", "ketplusBrowser"),
                                    activity("middle-2", "ketplusBrowser"),
                                    evidence,
                                    activity("after-1", "ketplusBrowser"),
                                    activity("after-2", "ketplusBrowser")};
            assignActivityGroups(rows);
            for (const int leader : {0, 3, 6})
                QCOMPARE(rows.at(leader).value("groupSize").toInt(), 2);
            QVERIFY(rows.at(2).value("groupRole").toString().isEmpty());
            QVERIFY(rows.at(5).value("groupRole").toString().isEmpty());
            // An empty/failed capture or comparison stays outside the group as well.
            rows[2].insert("running", false);
            rows[2].insert("status", "failed");
            assignActivityGroups(rows, 2, 2);
            QVERIFY(rows.at(2).value("groupRole").toString().isEmpty());
        }
    }

    // A command keeps what the agent said it is for, to read above the command itself.
    void commandsCarryTheirDescription() {
        const auto row = activityMessage(QJsonObject{{"id", "c"},
                                                     {"type", "commandExecution"},
                                                     {"command", "ssh host uptime"},
                                                     {"description", "  Check the runner\n host "},
                                                     {"status", "completed"}},
                                         true);
        QCOMPARE(row.value("commandDescription").toString(),
                 QStringLiteral("Check the runner host"));
        QCOMPARE(row.value("command").toString(), QStringLiteral("ssh host uptime"));
    }

    // A trailing pager names no file. Reading its flag as one titled the row
    // "Read -20" where the reader expected a file name.
    void readerTitlesNeedAFileOperandRatherThanAFlag_data() {
        QTest::addColumn<QString>("command");
        QTest::addColumn<QString>("subtype");
        QTest::addColumn<QString>("title");
        QTest::newRow("pager at the end of a pipeline")
            << "grep -rn Reading modules | head -20" << "command" << QString();
        QTest::newRow("pager before another command")
            << "grep -rn Reading modules | head -20; echo done" << "command" << QString();
        QTest::newRow("pager inside a substitution")
            << "ps -o pid -p $(pgrep -f daemon | head -1) 2>&1" << "command" << QString();
        QTest::newRow("file after a count flag")
            << "head -8 modules/ai/src/agent/AgentSessionHistory.cpp" << "fileRead"
            << "Read: AgentSessionHistory.cpp";
        QTest::newRow("file after a valued flag")
            << "head -n 20 src/main.cpp" << "fileRead" << "Read: main.cpp";
        QTest::newRow("file with no flag") << "cat /tmp/out.txt" << "fileRead" << "Read: out.txt";
    }

    void readerTitlesNeedAFileOperandRatherThanAFlag() {
        QFETCH(QString, command);
        QFETCH(QString, subtype);
        QFETCH(QString, title);
        const auto row = commandMessage(command, QStringLiteral("completed"), QStringLiteral("c"));
        QCOMPARE(row.value("subtype").toString(), subtype);
        if (!title.isEmpty())
            QCOMPARE(row.value("title").toString(), title);
        else
            QCOMPARE(row.value("title").toString(), QStringLiteral("Ran: ") + command);
    }
};

QTEST_GUILESS_MAIN(TranscriptActivityGroupsTest)
#include "TranscriptActivityGroupsTest.moc"
