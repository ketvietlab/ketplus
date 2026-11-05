#pragma once

#include "cloud/CloudEventProjection.h"
#include "storage/LocalJournal.h"
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

namespace r03 {

// Shared by the existing daemon protocol suite and the isolated Release runner.
inline void productionProjectionChecks() {
    using namespace ketplus::daemon;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalJournal journal(directory.filePath("synthetic.sqlite"));
    QVERIFY(journal.upsertSession({"commands", directory.path(), "synthetic", {}, {}, "ready"}));
    const QString marker = "KETPLUS_COMMAND_OUTPUT_SHOULD_NOT_REACH_CLOUD";
    const QString command = QString(180, QLatin1Char('x')) +
                            QString::fromUtf8(" \xE2\x9C\x93 \xE6\x97\xA5\xE6\x9C\xAC");
    for (const QString& status : {QString("inProgress"), QString("completed"), QString("failed")}) {
        const QString type = status == "inProgress" ? "item.started" : "item.completed";
        const QJsonObject payload{
            {"item_id", status},
            {"item_type", "commandExecution"},
            {"command", command},
            {"status", status},
            {"item", QJsonObject{{"id", status},
                                 {"type", "commandExecution"},
                                 {"command", command},
                                 {"status", status},
                                 {"exitCode", status == "failed" ? 7 : 0},
                                 {"aggregatedOutput", marker + "_AGGREGATED"},
                                 {"commandActions", QJsonArray{QJsonObject{{"output", marker}}}}}}};
        QVERIFY(journal.appendEvent("commands", 1, type, payload) > 0);
        const auto bytes = QJsonDocument(projectEventPayloadForCloud(type, payload))
                               .toJson(QJsonDocument::Compact);
        QVERIFY(!bytes.contains(marker.toUtf8()));
        const auto decoded = QJsonDocument::fromJson(bytes).object();
        QCOMPARE(decoded.value("command").toString(), command);
        QCOMPARE(decoded.value("status").toString(), status);
        const auto item = decoded.value("item").toObject();
        QCOMPARE(item.value("command").toString(), command);
        QCOMPARE(item.value("status").toString(), status);
        QCOMPARE(item.value("exitCode").toInt(), status == "failed" ? 7 : 0);
    }
    const QJsonObject delta{{"item_id", "completed"}, {"delta", marker}};
    QVERIFY(journal.appendEvent("commands", 1, "command.output_delta", delta) > 0);
    const auto bytes = QJsonDocument(projectEventPayloadForCloud("command.output_delta", delta))
                           .toJson(QJsonDocument::Compact);
    QVERIFY(!bytes.contains(marker.toUtf8()));
    QCOMPARE(QJsonDocument::fromJson(bytes).object().value("item_id").toString(),
             QString("completed"));
    QVERIFY(
        QJsonDocument(journal.eventsAfter("commands", 1, 0)).toJson().contains(marker.toUtf8()));
    const auto nonCommand = [&marker](const QString& itemType) {
        return QJsonObject{{"item", QJsonObject{{"id", "non-command"},
                                                {"type", itemType},
                                                {"text", marker},
                                                {"output", marker}}}};
    };
    const QJsonObject agentMessage = nonCommand("agentMessage");
    QCOMPARE(projectEventPayloadForCloud("item.completed", agentMessage), agentMessage);
    QCOMPARE(QJsonDocument(projectEventPayloadForCloud("item.completed", agentMessage))
                 .toJson(QJsonDocument::Compact),
             QJsonDocument(agentMessage).toJson(QJsonDocument::Compact));
    // MCP tool calls have an explicit metadata-only rule (media refs, capped text; ADR 0008).
    const QJsonObject mcp =
        projectEventPayloadForCloud("item.completed", nonCommand("mcpToolCall"));
    QCOMPARE(mcp.value("item").toObject().value("type").toString(), QString("mcpToolCall"));
    QVERIFY(!QJsonDocument(mcp).toJson(QJsonDocument::Compact).contains(marker.toUtf8()));
}

} // namespace r03
