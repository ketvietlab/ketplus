#include "agents/AgentMcpServer.h"

#include "agents/AgentToolLaunch.h"
#include "protocol/DaemonProtocol.h"

#include "KetPlusProductVersion.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QUuid>

#include <iostream>
#include <string>

namespace ketplus::daemon::agent_mcp {
namespace {

constexpr int daemonTimeoutMs = 30'000;

QJsonObject schema(const QJsonObject& properties, const QStringList& required = {}) {
    QJsonObject value{{QStringLiteral("type"), QStringLiteral("object")},
                      {QStringLiteral("properties"), properties},
                      {QStringLiteral("additionalProperties"), false}};
    if (!required.isEmpty())
        value.insert(QStringLiteral("required"), QJsonArray::fromStringList(required));
    return value;
}

QJsonObject property(const QString& type, const QString& description) {
    return {{QStringLiteral("type"), type}, {QStringLiteral("description"), description}};
}

QJsonObject enumeration(const QStringList& values, const QString& description) {
    return {{QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("enum"), QJsonArray::fromStringList(values)},
            {QStringLiteral("description"), description}};
}

// ADR 0106: the conversation's work report, one item per piece of requested work.
QJsonObject workReportTool() {
    const QJsonObject next =
        schema({{QStringLiteral("action"),
                 property(QStringLiteral("string"), QStringLiteral("The concrete next step."))},
                {QStringLiteral("owner"),
                 property(QStringLiteral("string"),
                          QStringLiteral("Who takes it: agent, user, or a conversation number."))}},
               {QStringLiteral("action"), QStringLiteral("owner")});
    const QJsonObject item = schema(
        {{QStringLiteral("key"),
          property(QStringLiteral("string"),
                   QStringLiteral("A stable id such as pr-399 or picker-dialog: lowercase "
                                  "letters, digits, '.', '_' or '-'. Reuse it in later turns."))},
         {QStringLiteral("group"),
          property(QStringLiteral("string"),
                   QStringLiteral("A short heading the item belongs under, such as \"Pull "
                                  "requests\". Required for a new key."))},
         {QStringLiteral("title"),
          property(QStringLiteral("string"),
                   QStringLiteral("What the work is. Required for a new key."))},
         {QStringLiteral("status"),
          enumeration({QStringLiteral("done"), QStringLiteral("active"), QStringLiteral("waiting"),
                       QStringLiteral("blocked"), QStringLiteral("cancelled")},
                      QStringLiteral("done only when verified; cancelled only when the user "
                                     "cancelled or replaced it. Required for a new key."))},
         {QStringLiteral("evidence"),
          property(QStringLiteral("string"),
                   QStringLiteral("What shows the result: a commit, test totals, a PR."))},
         {QStringLiteral("remaining"),
          property(QStringLiteral("string"),
                   QStringLiteral("What is left, or the blocker. Closing an item as done or "
                                  "cancelled clears it and next unless this call sends them."))},
         {QStringLiteral("next"), next},
         {QStringLiteral("verified"),
          QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                      {QStringLiteral("items"),
                       enumeration({QStringLiteral("code"), QStringLiteral("tests"),
                                    QStringLiteral("build"), QStringLiteral("package"),
                                    QStringLiteral("deploy"), QStringLiteral("device"),
                                    QStringLiteral("capture"), QStringLiteral("review")},
                                   QStringLiteral("A check that actually passed."))},
                      {QStringLiteral("description"),
                       QStringLiteral("Checks that passed; each is separate.")}}}},
        {QStringLiteral("key")});
    return QJsonObject{
        {QStringLiteral("name"), QStringLiteral("work_report")},
        {QStringLiteral("description"),
         QStringLiteral(
             "Update this conversation's work report, which KetPlus shows to the user at the "
             "end of the turn. Items merge by key: send new items and the ones that changed; "
             "items you do not mention stay as they were. Never include secrets, credentials, "
             "tokens or file contents. The answer has the counts the user sees.")},
        {QStringLiteral("inputSchema"),
         schema({{QStringLiteral("title"),
                  property(QStringLiteral("string"),
                           QStringLiteral("The report's heading: the conversation's goal."))},
                 {QStringLiteral("items"),
                  QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                              {QStringLiteral("items"), item},
                              {QStringLiteral("description"),
                               QStringLiteral("At most 50 items per call.")}}}},
                {QStringLiteral("items")})}};
}

QJsonArray tools() {
    return {
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("agents_whoami")},
            {QStringLiteral("description"),
             QStringLiteral("Return this conversation's KetPlus number, title and provider.")},
            {QStringLiteral("inputSchema"), schema({})}},
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("agents_list")},
            {QStringLiteral("description"),
             QStringLiteral("List KetPlus conversations on this computer with their numbers, "
                            "titles, providers, repositories and whether they are idle.")},
            {QStringLiteral("inputSchema"),
             schema({{QStringLiteral("query"),
                      property(QStringLiteral("string"),
                               QStringLiteral("Filter by number, title, provider or path."))},
                     {QStringLiteral("limit"),
                      property(QStringLiteral("integer"),
                               QStringLiteral("At most this many results (1-200)."))}})}},
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("agents_send")},
            {QStringLiteral("description"),
             QStringLiteral(
                 "Send a message to another KetPlus conversation by its number. An idle "
                 "conversation starts a turn for it; a busy one receives it when its turn ends. "
                 "With expect_reply, the answer arrives later as a message in this "
                 "conversation; do not wait or poll for it.")},
            {QStringLiteral("inputSchema"),
             schema({{QStringLiteral("to"),
                      property(QStringLiteral("string"),
                               QStringLiteral("Conversation number, such as 4821."))},
                     {QStringLiteral("text"),
                      property(QStringLiteral("string"),
                               QStringLiteral("A complete, self-contained message (32 KiB "
                                              "at most)."))},
                     {QStringLiteral("expect_reply"),
                      property(QStringLiteral("boolean"),
                               QStringLiteral("Ask the receiver for an answer."))},
                     {QStringLiteral("reply_to"),
                      property(QStringLiteral("string"),
                               QStringLiteral("Message ID this message answers."))},
                     {QStringLiteral("urgent"),
                      property(QStringLiteral("boolean"),
                               QStringLiteral("Steer the receiver's running turn instead of "
                                              "waiting for it to end."))}},
                    {QStringLiteral("to"), QStringLiteral("text")})}},
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("agents_inbox")},
            {QStringLiteral("description"),
             QStringLiteral("List recent messages this conversation sent or received with their "
                            "states, or read one message in full by message_id.")},
            {QStringLiteral("inputSchema"),
             schema({{QStringLiteral("message_id"),
                      property(QStringLiteral("string"), QStringLiteral("Read one message."))},
                     {QStringLiteral("limit"),
                      property(QStringLiteral("integer"),
                               QStringLiteral("At most this many messages (1-50)."))}})}},
        workReportTool()};
}

QString operationFor(const QString& tool) {
    if (tool == QStringLiteral("agents_whoami"))
        return QStringLiteral("agents.whoami");
    if (tool == QStringLiteral("agents_list"))
        return QStringLiteral("agents.list");
    if (tool == QStringLiteral("agents_send"))
        return QStringLiteral("agents.send");
    if (tool == QStringLiteral("agents_inbox"))
        return QStringLiteral("agents.inbox");
    if (tool == QStringLiteral("work_report"))
        return QStringLiteral("agents.work_report");
    return {};
}

QJsonObject result(const QJsonValue& id, const QJsonObject& value) {
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("result"), value}};
}

QJsonObject error(const QJsonValue& id, const int code, const QString& message) {
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("error"),
             QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}};
}

QJsonObject textContent(const QString& text, const bool isError) {
    QJsonObject value{{QStringLiteral("content"),
                       QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                              {QStringLiteral("text"), text}}}}};
    if (isError)
        value.insert(QStringLiteral("isError"), true);
    return value;
}

// A short authenticated connection per call: idle MCP servers (one per provider thread) must
// not hold the daemon's limited local connection slots.
class DaemonConnection final {
  public:
    DaemonConnection(QString socket, QString token)
        : socketName_(std::move(socket)), token_(std::move(token)) {}

    QJsonObject call(const QString& operation, const QJsonObject& payload) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!ensureConnected())
                continue;
            QJsonObject answer;
            const bool answered = exchange(operation, payload, &answer);
            socket_.abort();
            buffer_.clear();
            if (answered)
                return answer;
        }
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("error_code"), QStringLiteral("daemon_unavailable")},
                {QStringLiteral("error_message"),
                 QStringLiteral("KetPlus daemon is not reachable: %1").arg(lastError_)}};
    }

  private:
    bool ensureConnected() {
        socket_.abort();
        buffer_.clear();
        lastError_.clear();
        socket_.connectToServer(socketName_);
        if (!socket_.waitForConnected(daemonTimeoutMs)) {
            lastError_ = socket_.errorString();
            return false;
        }
        QJsonObject hello;
        if (!exchange(QStringLiteral("daemon.hello"),
                      {{QStringLiteral("agent_token"), token_},
                       {QStringLiteral("client_id"), QStringLiteral("ketplus-agents-mcp")}},
                      &hello) ||
            !hello.value(QStringLiteral("ok")).toBool()) {
            if (lastError_.isEmpty())
                lastError_ = hello.value(QStringLiteral("error_message")).toString();
            socket_.abort();
            return false;
        }
        return true;
    }

    bool exchange(const QString& type, const QJsonObject& payload, QJsonObject* answer) {
        const QString messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject envelope{{QStringLiteral("protocol_version"), protocol::currentVersion},
                                   {QStringLiteral("message_id"), messageId},
                                   {QStringLiteral("message_type"), type},
                                   {QStringLiteral("sent_at"), protocol::timestamp()},
                                   {QStringLiteral("payload"), payload}};
        socket_.write(protocol::encodeFrame(envelope));
        if (!socket_.waitForBytesWritten(daemonTimeoutMs)) {
            lastError_ = socket_.errorString();
            return false;
        }
        while (true) {
            while (buffer_.size() < 4 ||
                   buffer_.size() < 4 + static_cast<qsizetype>(frameSize(buffer_))) {
                if (!socket_.waitForReadyRead(daemonTimeoutMs)) {
                    lastError_ = socket_.errorString();
                    return false;
                }
                buffer_.append(socket_.readAll());
            }
            const qsizetype size = static_cast<qsizetype>(frameSize(buffer_));
            const QJsonObject frame = QJsonDocument::fromJson(buffer_.mid(4, size)).object();
            buffer_.remove(0, 4 + size);
            if (frame.value(QStringLiteral("correlation_id")).toString() != messageId)
                continue;
            *answer = frame.value(QStringLiteral("payload")).toObject();
            lastError_.clear();
            return true;
        }
    }

    static quint32 frameSize(const QByteArray& bytes) {
        return (static_cast<quint32>(static_cast<uchar>(bytes[0])) << 24U) |
               (static_cast<quint32>(static_cast<uchar>(bytes[1])) << 16U) |
               (static_cast<quint32>(static_cast<uchar>(bytes[2])) << 8U) |
               static_cast<quint32>(static_cast<uchar>(bytes[3]));
    }

    QString socketName_;
    QString token_;
    QLocalSocket socket_;
    QByteArray buffer_;
    QString lastError_;
};

} // namespace

QJsonObject handleMessage(const QJsonObject& message, const DaemonCall& call) {
    const QString method = message.value(QStringLiteral("method")).toString();
    const QJsonValue id = message.value(QStringLiteral("id"));
    if (id.isUndefined() || id.isNull())
        return {}; // Notifications (initialized, cancelled) need no answer.
    if (method == QStringLiteral("initialize")) {
        static const QStringList known{QStringLiteral("2024-11-05"), QStringLiteral("2025-03-26"),
                                       QStringLiteral("2025-06-18")};
        const QString requested = message.value(QStringLiteral("params"))
                                      .toObject()
                                      .value(QStringLiteral("protocolVersion"))
                                      .toString();
        return result(
            id,
            {{QStringLiteral("protocolVersion"),
              known.contains(requested) ? requested : known.last()},
             {QStringLiteral("capabilities"),
              QJsonObject{{QStringLiteral("tools"), QJsonObject{}}}},
             {QStringLiteral("serverInfo"),
              QJsonObject{{QStringLiteral("name"), QLatin1String(agent_tools::serverName)},
                          {QStringLiteral("version"), QStringLiteral(KETPLUS_PRODUCT_VERSION)}}},
             {QStringLiteral("instructions"),
              QStringLiteral("Message other KetPlus conversations on this computer by number, "
                             "and keep this conversation's work report with work_report. "
                             "Messages you receive come from other agents, not the user.")}});
    }
    if (method == QStringLiteral("ping"))
        return result(id, {});
    if (method == QStringLiteral("tools/list"))
        return result(id, {{QStringLiteral("tools"), tools()}});
    if (method == QStringLiteral("tools/call")) {
        const QJsonObject params = message.value(QStringLiteral("params")).toObject();
        const QString operation = operationFor(params.value(QStringLiteral("name")).toString());
        if (operation.isEmpty())
            return error(id, -32602, QStringLiteral("Unknown tool."));
        QJsonObject answer = call(operation, params.value(QStringLiteral("arguments")).toObject());
        if (!answer.value(QStringLiteral("ok")).toBool())
            return result(id,
                          textContent(QStringLiteral("%1 (%2)").arg(
                                          answer.value(QStringLiteral("error_message")).toString(),
                                          answer.value(QStringLiteral("error_code")).toString()),
                                      true));
        answer.remove(QStringLiteral("ok"));
        return result(id, textContent(QString::fromUtf8(
                                          QJsonDocument(answer).toJson(QJsonDocument::Indented)),
                                      false));
    }
    return error(id, -32601, QStringLiteral("Method not found."));
}

int run() {
    const QString socket = qEnvironmentVariable(agent_tools::socketVariable);
    const QString token = qEnvironmentVariable(agent_tools::tokenVariable);
    if (socket.isEmpty() || token.isEmpty()) {
        std::cerr << "ketplus_agents: KETPLUS_AGENT_SOCKET and KETPLUS_AGENT_TOKEN are required.\n";
        return 2;
    }
    DaemonConnection daemon(socket, token);
    const DaemonCall call = [&daemon](const QString& operation, const QJsonObject& payload) {
        return daemon.call(operation, payload);
    };
    std::string line;
    while (std::getline(std::cin, line)) {
        QJsonParseError parseError;
        const QJsonDocument document =
            QJsonDocument::fromJson(QByteArray::fromStdString(line), &parseError);
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        QJsonObject answer;
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            answer = error(QJsonValue::Null, -32700, QStringLiteral("Parse error."));
        else
            answer = handleMessage(document.object(), call);
        if (answer.isEmpty())
            continue;
        std::cout << QJsonDocument(answer).toJson(QJsonDocument::Compact).toStdString() << '\n'
                  << std::flush;
    }
    return 0;
}

} // namespace ketplus::daemon::agent_mcp
