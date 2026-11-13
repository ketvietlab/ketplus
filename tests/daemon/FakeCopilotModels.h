#pragma once
#include "providers/CopilotCatalog.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QThread>

inline QJsonObject copilotModelFixture(const QString& mode, const QString& current = "model-a") {
    if (mode == "missing")
        return {};
    if (mode == "legacy")
        return {{"models",
                 QJsonObject{{"currentModelId", current},
                             {"availableModels",
                              QJsonArray{QJsonObject{{"modelId", "model-a"}, {"name", "Alpha"}},
                                         QJsonObject{{"modelId", "model-b"}, {"name", "Beta"}}}}}}};
    QJsonArray options{QJsonObject{{"value", "model-a"}, {"name", "Alpha"}},
                       QJsonObject{{"value", "model-b"}, {"name", "Beta"}}};
    if (mode == "too-many")
        for (int i = 0; i < 256; ++i)
            options.append(QJsonObject{{"value", QString::number(i)}, {"name", "Extra"}});
    return {{"configOptions", QJsonArray{QJsonObject{{"id", "vendor-model"},
                                                     {"category", "model"},
                                                     {"type", "select"},
                                                     {"currentValue", current},
                                                     {"options", options}}}}};
}

inline int runFakeCopilotModels() {
    const QString mode = qEnvironmentVariable("KETPLUS_TEST_COPILOT_MODEL_SHAPE");
    const QString logPath = qEnvironmentVariable("KETPLUS_TEST_COPILOT_MODEL_LOG");
    QFile input, output, error;
    if (!input.open(stdin, QIODevice::ReadOnly) || !output.open(stdout, QIODevice::WriteOnly) ||
        !error.open(stderr, QIODevice::WriteOnly))
        return 2;
    const auto record = [&](const QJsonObject& value) {
        QFile log(logPath);
        if (log.open(QIODevice::WriteOnly | QIODevice::Append))
            log.write(QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n');
    };
    qsizetype written = 0;
    const auto emitFrame = [&](const QJsonObject& value) {
        const auto frame = QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
        written += frame.size();
        output.write(frame);
        output.flush();
    };
    record({{"launched", true},
            {"argv", QJsonArray::fromStringList(QCoreApplication::arguments().mid(1))}});
    QString current = "model-a";
    int sessions = 0;
    while (true) {
        const QByteArray line = input.readLine();
        if (line.isEmpty())
            break;
        const QJsonObject message = QJsonDocument::fromJson(line).object();
        record(message);
        const auto id = message.value("id");
        const auto method = message.value("method").toString();
        const auto params = message.value("params").toObject();
        QJsonObject result;
        if (method == "initialize") {
            if (mode == "malformed") {
                output.write("not-json\n");
                output.flush();
                continue;
            }
            if (mode == "wrong-id") {
                emitFrame({{"jsonrpc", "2.0"}, {"id", "1"}, {"result", QJsonObject{}}});
                continue;
            }
            if (mode == "timeout") {
                QThread::sleep(10);
                continue;
            }
            if (mode == "flood-stdout" || mode == "flood-stderr") {
                auto& channel = mode == "flood-stdout" ? output : error;
                for (int i = 0; i < 1024; ++i) {
                    if (channel.write(QByteArray(64 * 1024, 'x')) < 0 || !channel.flush())
                        return 2;
                }
                record({{"floodCompleted", true}});
            }
            if (mode == "error") {
                emitFrame({{"jsonrpc", "2.0"},
                           {"id", id},
                           {"error", QJsonObject{{"code", -32000},
                                                 {"message", "Authentication required"}}}});
                continue;
            }
            result = {{"protocolVersion", mode == "unsupported-version" ? 2 : 1},
                      {"agentCapabilities", QJsonObject{{"loadSession", mode != "no-load"}}}};
        } else if (method == "session/new") {
            current = "model-a";
            result = copilotModelFixture(mode, current);
            result.insert("sessionId", QStringLiteral("fixture-%1").arg(++sessions));
            if (mode == "request-permission") {
                emitFrame({{"jsonrpc", "2.0"},
                           {"id", 99},
                           {"method", "session/request_permission"},
                           {"params", QJsonObject{}}});
                continue;
            }
        } else if (method == "session/load") {
            if (mode != "empty-load" && mode != "update")
                result = copilotModelFixture(mode, current);
        } else if (method == "session/set_config_option" || method == "session/set_model") {
            if (mode == "reject") {
                emitFrame({{"jsonrpc", "2.0"},
                           {"id", id},
                           {"error", QJsonObject{{"code", -32601},
                                                 {"message", "Model switching unsupported"}}}});
                continue;
            }
            if (mode != "unconfirmed")
                current =
                    params.value(method == "session/set_model" ? "modelId" : "value").toString();
            if (method == "session/set_config_option")
                result = copilotModelFixture(mode, current);
        } else if (method == "session/prompt") {
            record({{"promptModel", current}});
            if (mode == "update") {
                current = "model-a";
                auto update = copilotModelFixture("config", current);
                update.insert("sessionUpdate", "config_option_update");
                emitFrame({{"jsonrpc", "2.0"},
                           {"method", "session/update"},
                           {"params", QJsonObject{{"sessionId", params.value("sessionId")},
                                                  {"update", update}}}});
            }
            result = {{"stopReason", "end_turn"}};
        }
        const QJsonObject response{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
        if (method == "session/new" && (mode.endsWith("boundary") || mode.endsWith("overflow"))) {
            const auto padding = ketplus::daemon::copilot_catalog::maximumOutputBytes - written -
                                 QJsonDocument(response).toJson(QJsonDocument::Compact).size() - 1 +
                                 (mode.endsWith("overflow") ? 1 : 0);
            if (mode.startsWith("stdout")) {
                output.write(QByteArray(padding - 1, ' ') + '\n');
                output.flush();
            } else {
                error.write(QByteArray(padding, 'x'));
                error.flush();
            }
            written += padding;
        }
        emitFrame(response);
    }
    return 0;
}
