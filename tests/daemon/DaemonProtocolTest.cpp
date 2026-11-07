#include "protocol/DaemonProtocol.h"
#include "characterization/ProjectionChecks.h"
#include "cloud/CloudControlBatching.h"
#include "cloud/WorktreeBranches.h"
#include "cloud/WorktreeRepositories.h"
#include "cloud/CloudEventProjection.h"
#include "providers/CodexAdapter.h"
#include "runtime/ApprovalBroker.h"
#include "runtime/SessionHost.h"
#include "service/DaemonService.h"
#include "storage/LocalJournal.h"
#include "storage/McpMediaStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace ketplus::daemon;

class DaemonProtocolTest final : public QObject {
    Q_OBJECT

  private slots:
    // DaemonService reads the machine's cloud credentials from the user's data directory, not
    // from the temporary state it is given. Without test paths, a run from a shell that set
    // KETPLUS_CONTROL_URL published these fixtures as live conversations of the real machine.
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    // Turns run on the conversation's own agent, so its settings list only that agent's
    // models, even when another agent lists the same id.
    void conversationSettingsOfferOnlyTheConversationsAgent() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        StoredSession stored;
        stored.id = QStringLiteral("codex-session");
        stored.worktreePath = QFileInfo(directory.path()).canonicalFilePath();
        stored.adapter = QStringLiteral("codex");
        stored.state = QStringLiteral("ready");
        QVERIFY(journal.upsertSession(stored));
        CodexAdapter adapter(QStringLiteral("/nonexistent/ketplus-test-codex"));
        SessionHost host(&journal, &adapter);
        const auto entry = [](const QString& provider, const QString& id, const QString& effort) {
            return QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("label"), id},
                {QStringLiteral("provider"), provider},
                {QStringLiteral("efforts"),
                 QJsonArray{QJsonObject{{QStringLiteral("id"), effort},
                                        {QStringLiteral("label"), effort}}}}};
        };
        const QJsonObject codexModel = entry("codex", "shared-model", "medium");
        const QJsonArray merged{codexModel, entry("claude", "claude-model", "high"),
                                entry("cursor", "shared-model", "max")};
        const auto request = [&](const QString& type, QJsonObject payload) {
            payload.insert(QStringLiteral("provider_catalog"), merged);
            return host.handleCommand(
                {{QStringLiteral("protocol_version"), 0},
                 {QStringLiteral("message_id"), QUuid::createUuid().toString()},
                 {QStringLiteral("message_type"), type},
                 {QStringLiteral("session_id"), QStringLiteral("codex-session")},
                 {QStringLiteral("session_generation"), 1},
                 {QStringLiteral("payload"), payload}},
                QStringLiteral("test"));
        };
        const QJsonObject read = request(QStringLiteral("session.configuration.read"), {});
        QCOMPARE(read.value(QStringLiteral("models")).toArray(), QJsonArray{codexModel});
        // The agents it can move to between turns come apart, each naming its agent.
        QCOMPARE(read.value(QStringLiteral("adapter")).toString(), QStringLiteral("codex"));
        QCOMPARE(read.value(QStringLiteral("agent_models")).toArray(), merged);
        const QString revision = read.value(QStringLiteral("catalog_revision")).toString();
        const auto apply = [&](const QString& model, const QString& effort) {
            return request(QStringLiteral("session.configuration.apply"),
                           {{QStringLiteral("expected_revision"), 1},
                            {QStringLiteral("expected_catalog_revision"), revision},
                            {QStringLiteral("model"), model},
                            {QStringLiteral("effort"), effort}});
        };
        QCOMPARE(apply("claude-model", "high").value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("unsupported_configuration"));
        QCOMPARE(apply("shared-model", "max").value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("unsupported_configuration"));
        const QJsonObject applied = apply("shared-model", "medium");
        QVERIFY2(applied.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(applied).toJson()));
    }

    void aConversationStartedFromThePhoneBeginsWithTheSettingsItChose() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        CodexAdapter adapter(QStringLiteral("/nonexistent/ketplus-test-codex"));
        SessionHost host(&journal, &adapter);
        // Pending provider callbacks capture the host; drain them before it is destroyed.
        const auto stopProvider = qScopeGuard([&adapter] { adapter.stop(); });
        const QJsonObject codexModel{
            {QStringLiteral("id"), QStringLiteral("codex-model")},
            {QStringLiteral("label"), QStringLiteral("Codex model")},
            {QStringLiteral("provider"), QStringLiteral("codex")},
            {QStringLiteral("efforts"),
             QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("high")},
                                    {QStringLiteral("label"), QStringLiteral("High")}}}}};
        const QJsonArray merged{codexModel, QJsonObject{{QStringLiteral("id"), QStringLiteral("claude-model")},
                                                        {QStringLiteral("label"), QStringLiteral("Claude model")},
                                                        {QStringLiteral("provider"), QStringLiteral("claude")},
                                                        {QStringLiteral("efforts"), QJsonArray{}}}};
        // Before the first message the phone reads what a new conversation starts with.
        const QJsonObject offered = host.newConversationConfiguration(merged);
        QVERIFY(offered.value(QStringLiteral("ok")).toBool());
        // Every agent's models, since the phone picks the agent of a new conversation.
        QCOMPARE(offered.value(QStringLiteral("models")).toArray(), merged);
        QCOMPARE(offered.value(QStringLiteral("revision")).toInteger(), 1);
        QCOMPARE(offered.value(QStringLiteral("effective"))
                     .toObject()
                     .value(QStringLiteral("execution_policy"))
                     .toString(),
                 QStringLiteral("ask"));
        // The phone reads every effective setting as a string, so a new conversation's
        // unchosen model and effort go out as empty strings, not null.
        const QJsonObject offeredEffective = offered.value(QStringLiteral("effective")).toObject();
        QVERIFY(offeredEffective.value(QStringLiteral("model")).isString());
        QVERIFY(offeredEffective.value(QStringLiteral("effort")).isString());
        QVERIFY(QJsonDocument(offered).toJson(QJsonDocument::Compact).contains("\"model\":\"\""));

        const QString worktree = QFileInfo(directory.path()).canonicalFilePath();
        const auto start = [&](const QString& sessionId, const QJsonObject& options) {
            return host.handleCommand(
                {{QStringLiteral("protocol_version"), 0},
                 {QStringLiteral("message_id"), QUuid::createUuid().toString()},
                 {QStringLiteral("message_type"), QStringLiteral("session.start_turn")},
                 {QStringLiteral("session_id"), sessionId},
                 {QStringLiteral("payload"),
                  QJsonObject{{QStringLiteral("prompt"), QStringLiteral("hello")},
                              {QStringLiteral("worktree_path"), worktree},
                              {QStringLiteral("options"), options}}}},
                QStringLiteral("test"));
        };
        const QJsonObject acknowledged =
            start(QStringLiteral("full-session"),
                  {{QStringLiteral("model"), QStringLiteral("codex-model")},
                   {QStringLiteral("effort"), QStringLiteral("high")},
                   {QStringLiteral("execution_policy"), QStringLiteral("full")},
                   {QStringLiteral("risk_acknowledged"), true}});
        QVERIFY2(acknowledged.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(acknowledged).toJson()));
        // The agent chosen on the phone runs the conversation; this test Mac has no Claude.
        QCOMPARE(start(QStringLiteral("claude-session"),
                       {{QStringLiteral("provider"), QStringLiteral("claude")},
                        {QStringLiteral("model"), QStringLiteral("claude-model")}})
                     .value(QStringLiteral("error_code"))
                     .toString(),
                 QStringLiteral("provider_unavailable"));
        // Full access without the risk acknowledgement stays on asking.
        QVERIFY(start(QStringLiteral("unacknowledged-session"),
                      {{QStringLiteral("execution_policy"), QStringLiteral("full")}})
                    .value(QStringLiteral("ok"))
                    .toBool());
        const auto effective = [&](const QString& sessionId) {
            return host
                .handleCommand(
                    {{QStringLiteral("protocol_version"), 0},
                     {QStringLiteral("message_id"), QUuid::createUuid().toString()},
                     {QStringLiteral("message_type"), QStringLiteral("session.configuration.read")},
                     {QStringLiteral("session_id"), sessionId},
                     {QStringLiteral("session_generation"), 1},
                     {QStringLiteral("payload"),
                      QJsonObject{{QStringLiteral("provider_catalog"), merged}}}},
                    QStringLiteral("test"))
                .value(QStringLiteral("effective"))
                .toObject();
        };
        const QJsonObject chosen = effective(QStringLiteral("full-session"));
        QCOMPARE(chosen.value(QStringLiteral("execution_policy")).toString(), QStringLiteral("full"));
        QCOMPARE(chosen.value(QStringLiteral("model")).toString(), QStringLiteral("codex-model"));
        QCOMPARE(chosen.value(QStringLiteral("effort")).toString(), QStringLiteral("high"));
        QCOMPARE(effective(QStringLiteral("unacknowledged-session"))
                     .value(QStringLiteral("execution_policy"))
                     .toString(),
                 QStringLiteral("ask"));
    }

    // A phone names the file a Read or Edit touched, so a deep path keeps its tail, and
    // an Edit's replaced and replacement text never leave the desktop.
    void fileToolArgumentsKeepTheFileNameAndDropEditedText() {
        const QString path =
            "/Users/synthetic/" + QString(90, 'd') + "/Sources/LiveControlStore.swift";
        const QString summary = cloudArgumentSummary(
            {{"file_path", path}, {"old_string", "OLD_MARKER"}, {"new_string", "NEW_MARKER"}});
        QVERIFY2(summary.startsWith(QStringLiteral("file_path=") + QChar(0x2026)),
                 qPrintable(summary));
        QVERIFY2(summary.endsWith("/Sources/LiveControlStore.swift"), qPrintable(summary));
        QVERIFY(!summary.contains("MARKER"));
    }

    void advancedConfigurationAndAttachmentsAreRevisionFencedAndDurable() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY(journal.isOpen());
        StoredSession stored;
        stored.id = QStringLiteral("advanced-session");
        stored.worktreePath = QFileInfo(directory.path()).canonicalFilePath();
        stored.adapter = QStringLiteral("codex");
        stored.state = QStringLiteral("ready");
        QVERIFY(journal.upsertSession(stored));
        CodexAdapter adapter(QStringLiteral("/nonexistent/ketplus-test-codex"));
        SessionHost host(&journal, &adapter);
        const auto request = [](const QString& type, const QJsonObject& payload) {
            return QJsonObject{{QStringLiteral("protocol_version"), 0},
                               {QStringLiteral("message_id"), QUuid::createUuid().toString()},
                               {QStringLiteral("message_type"), type},
                               {QStringLiteral("session_id"), QStringLiteral("advanced-session")},
                               {QStringLiteral("session_generation"), 1},
                               {QStringLiteral("payload"), payload}};
        };
        const QJsonArray models{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("provider-model")},
            {QStringLiteral("label"), QStringLiteral("Provider model")},
            {QStringLiteral("default_effort"), QStringLiteral("medium")},
            {QStringLiteral("efforts"),
             QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("medium")},
                                    {QStringLiteral("label"), QStringLiteral("Medium")}}}}}};
        const QJsonObject staleCatalog = host.handleCommand(
            request(QStringLiteral("session.configuration.apply"),
                    {{QStringLiteral("expected_revision"), 1},
                     {QStringLiteral("expected_catalog_revision"), QString(64, QLatin1Char('0'))},
                     {QStringLiteral("model"), QStringLiteral("provider-model")},
                     {QStringLiteral("effort"), QStringLiteral("medium")},
                     {QStringLiteral("provider_catalog"), models}}),
            QStringLiteral("test"));
        QCOMPARE(staleCatalog.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("stale_model_catalog"));
        const QJsonObject configured = host.handleCommand(
            request(QStringLiteral("session.configuration.apply"),
                    {{QStringLiteral("expected_revision"), 1},
                     {QStringLiteral("expected_catalog_revision"),
                      QString::fromLatin1(QCryptographicHash::hash(
                                              QJsonDocument(models).toJson(QJsonDocument::Compact),
                                              QCryptographicHash::Sha256)
                                              .toHex())},
                     {QStringLiteral("model"), QStringLiteral("provider-model")},
                     {QStringLiteral("effort"), QStringLiteral("medium")},
                     {QStringLiteral("provider_catalog"), models}}),
            QStringLiteral("test"));
        QVERIFY2(configured.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(configured).toJson()));
        QCOMPARE(configured.value(QStringLiteral("revision")).toInteger(), 2);
        const QJsonObject stale = host.handleCommand(
            request(QStringLiteral("session.execution_policy.apply"),
                    {{QStringLiteral("expected_revision"), 1},
                     {QStringLiteral("execution_policy"), QStringLiteral("full")},
                     {QStringLiteral("risk_acknowledged"), true}}),
            QStringLiteral("test"));
        QCOMPARE(stale.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("stale_configuration"));
        const QByteArray bytes("private attachment bytes");
        const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        QVERIFY(journal.beginAttachment("advanced-session", 1, "attachment-1", 0, "note.txt",
                                        "text/plain", bytes.size(), 1, digest));
        QVERIFY(journal.appendAttachmentChunk("advanced-session", 1, "attachment-1", 1, 0, bytes,
                                              digest));
        QVERIFY(journal.commitAttachment("advanced-session", 1, "attachment-1", 1));
        const StagedAttachment attachment =
            journal.attachment("advanced-session", 1, "attachment-1");
        QCOMPARE(attachment.status, QStringLiteral("ready"));
        QCOMPARE(attachment.bytes, bytes);
        const QJsonObject duplicateReferences = host.handleCommand(
            request(QStringLiteral("session.start_turn"),
                    {{QStringLiteral("prompt"), QStringLiteral("use attachment")},
                     {QStringLiteral("worktree_path"), stored.worktreePath},
                     {QStringLiteral("attachment_ids"),
                      QJsonArray{QStringLiteral("attachment-1"), QStringLiteral("attachment-1")}}}),
            QStringLiteral("test"));
        QCOMPARE(duplicateReferences.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("invalid_attachment_reference"));

        LocalJournal reopened(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY(reopened.isOpen());
        const auto sessions = reopened.sessions();
        QCOMPARE(sessions.size(), 1);
        QCOMPARE(sessions.first().model, QStringLiteral("provider-model"));
        QCOMPARE(sessions.first().configurationRevision, 2);
    }

    void expiredAttachmentStagingIsGarbageCollectedOnOpen() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("journal.sqlite3"));
        const QByteArray bytes("expired");
        const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        {
            LocalJournal journal(path);
            QVERIFY(journal.isOpen());
            StoredSession stored;
            stored.id = QStringLiteral("expiry-session");
            stored.worktreePath = directory.path();
            stored.adapter = QStringLiteral("codex");
            stored.state = QStringLiteral("ready");
            QVERIFY(journal.upsertSession(stored));
            QVERIFY(journal.beginAttachment("expiry-session", 1, "attachment-expired", 0,
                                            "note.txt", "text/plain", bytes.size(), 1, digest));
            QVERIFY(journal.appendAttachmentChunk("expiry-session", 1, "attachment-expired", 1, 0,
                                                  bytes, digest));
        }
        const QString connection = QStringLiteral("attachment-expiry-test");
        {
            QSqlDatabase database =
                QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(path);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral(
                "UPDATE staged_attachments SET created_at='2020-01-01T00:00:00.000Z'")));
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        LocalJournal reopened(path);
        QVERIFY(reopened.isOpen());
        QVERIFY(reopened.attachment("expiry-session", 1, "attachment-expired").id.isEmpty());
    }

    void heartbeatSnapshotsUseBoundedLosslessBatches() {
        QJsonArray sessions;
        for (int index = 0; index < 205; ++index) {
            sessions.append(QJsonObject{{"id", index}});
        }
        const QList<QJsonArray> batches = heartbeatSessionBatches(sessions);
        QCOMPARE(batches.size(), 3);
        QCOMPARE(batches.at(0).size(), 100);
        QCOMPARE(batches.at(1).size(), 100);
        QCOMPARE(batches.at(2).size(), 5);
        QJsonArray recovered;
        for (const QJsonArray& batch : batches) {
            for (const QJsonValue& value : batch) {
                recovered.append(value);
            }
        }
        QCOMPARE(recovered, sessions);
        const QList<QJsonArray> empty = heartbeatSessionBatches({});
        QCOMPARE(empty.size(), 1);
        QVERIFY(empty.first().isEmpty());
    }

    void eventUploadWaitDoublesUpToItsCap() {
        QCOMPARE(eventUploadBackoffMilliseconds(1, true), 1'000);
        QCOMPARE(eventUploadBackoffMilliseconds(2, true), 2'000);
        QCOMPARE(eventUploadBackoffMilliseconds(9, true), 256'000);
        QCOMPARE(eventUploadBackoffMilliseconds(10, true), 300'000);
        QCOMPARE(eventUploadBackoffMilliseconds(1'000, true), 300'000);
        QCOMPARE(eventUploadBackoffMilliseconds(6, false), 32'000);
        QCOMPARE(eventUploadBackoffMilliseconds(7, false), 60'000);
        QCOMPARE(eventUploadBackoffMilliseconds(1'000, false), 60'000);
        for (const int status : {400, 403, 404, 409, 413, 422})
            QVERIFY(eventUploadRefused(status));
        for (const int status : {0, 200, 202, 408, 429, 500, 502, 503})
            QVERIFY(!eventUploadRefused(status));
    }

    void heartbeatReplyNamesTheSessionsTheRelayRefused() {
        const QJsonObject reply{
            {"ok", true},
            {"accepted", true},
            {"rejectedSessions", QJsonArray{QJsonObject{{"sessionId", "owned-elsewhere"},
                                                        {"code", "session_owned_by_other_machine"}},
                                            QJsonObject{{"sessionId", "ahead"},
                                                        {"code", "session_generation_conflict"}},
                                            QJsonObject{{"code", "missing-id"}}}}};
        QCOMPARE(heartbeatRefusedSessions(reply), (QSet<QString>{"owned-elsewhere", "ahead"}));
        QVERIFY(heartbeatRefusedSessions(QJsonObject{{"ok", true}, {"accepted", true}}).isEmpty());
        QVERIFY(heartbeatRefusedSessions(QJsonValue{}).isEmpty());
    }

    void extendedProductionProjectionCharacterization() { r03::productionProjectionChecks(); }

    static QByteArray fakePng(const quint32 width, const quint32 height, const char salt) {
        QByteArray png("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR", 16);
        for (const quint32 value : {width, height}) {
            for (int shift = 24; shift >= 0; shift -= 8)
                png.append(static_cast<char>((value >> shift) & 0xff));
        }
        png.append("\x08\x06\0\0\0\0\0\0\0", 9);
        png.append(salt);
        return png;
    }

    static bool containsKey(const QJsonValue& value, const QString& key) {
        if (value.isObject()) {
            const QJsonObject object = value.toObject();
            if (object.contains(key))
                return true;
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (containsKey(it.value(), key))
                    return true;
            }
        } else if (value.isArray()) {
            for (const QJsonValue& child : value.toArray()) {
                if (containsKey(child, key))
                    return true;
            }
        }
        return false;
    }

    void localImageActivitiesSurviveSourceCleanup() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        McpMediaStore store(directory.filePath("media"));
        const QByteArray png = fakePng(8, 6, 'p');
        for (const QString& type :
             {QStringLiteral("imageView"), QStringLiteral("imageGeneration")}) {
            const QString key = type == "imageView" ? "path" : "savedPath";
            const QString path = directory.filePath("browser capture.png");
            QFile source(path);
            QVERIFY(source.open(QIODevice::WriteOnly));
            QCOMPARE(source.write(png), png.size());
            source.close();
            const QJsonObject item =
                store.externalizeItem({{"id", type}, {"type", type}, {key, "browser capture.png"}},
                                      "session", directory.path());
            QVERIFY(QFile::remove(path));
            const QJsonObject block =
                item.value("result").toObject().value("content").toArray().first().toObject();
            QVERIFY(!block.contains("data"));
            QCOMPARE(block.value("width").toInt(), 8);
            QCOMPARE(block.value("height").toInt(), 6);
            const QString hex = block.value("media_ref").toString().mid(7);
            QCOMPARE(hex, QString::fromLatin1(
                              QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex()));
            QFile retained(directory.filePath("media/" + hex + ".png"));
            QVERIFY(retained.open(QIODevice::ReadOnly));
            QCOMPARE(retained.readAll(), png);
            // The existing relay/mobile envelope carries the same reference, never file bytes.
            const auto projected = projectEventPayloadForCloud("item.completed", {{"item", item}});
            QCOMPARE(projected.value("item").toObject().value("result"), item.value("result"));
        }
        store.copySessionReferences("session", "fork");
        store.releaseSession("session");
        QCOMPARE(QDir(store.directory()).entryList({"*.png"}, QDir::Files).size(), 1);
        store.releaseSession("fork");
        QVERIFY(QDir(store.directory()).entryList({"*.png"}, QDir::Files).isEmpty());
    }

    void localImageActivitiesRejectUnavailableAndOversizedFiles() {
        QTemporaryDir directory;
        McpMediaStore store(directory.filePath("media"));
        const auto result = [&](const QString& path) {
            return store.externalizeItem({{"type", "imageView"}, {"path", path}}, "session")
                .value("result")
                .toObject()
                .value("content")
                .toArray()
                .first()
                .toObject();
        };
        QCOMPARE(result(directory.filePath("missing.png")).value("reason").toString(),
                 "source_unavailable");
        QCOMPARE(result(directory.path()).value("reason").toString(), "source_unavailable");
        QCOMPARE(result("relative.png").value("reason").toString(), "source_unavailable");
        QFile source(directory.filePath("large.png"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        QVERIFY(source.resize(McpMediaStore::maximumImageBytes + 1));
        source.close();
        QCOMPARE(result(source.fileName()).value("reason").toString(), "too_large");
        QVERIFY(!QFileInfo::exists(store.directory()));
    }

    void mcpMediaIsStoredOwnerOnlyAndReferencedFromPayloads() {
        QTemporaryDir directory;
        const QString mediaDirectory = directory.filePath("state/media");
        McpMediaStore store(mediaDirectory);
        const QByteArray png = fakePng(640, 480, 'a');
        const QString base64 = QString::fromLatin1(png.toBase64());
        const QJsonObject image{{"type", "image"}, {"data", base64}, {"mimeType", "image/png"}};
        QJsonArray content{
            QJsonObject{{"type", "text"}, {"text", "{\"ok\":true}"}}, image, image,
            QJsonObject{{"type", "audio"},
                        {"data", "QVVESU9fQllURVNfU0hPVUxEX05PVF9TVEFZ"},
                        {"mimeType", "audio/wav"}},
            QJsonObject{{"type", "image"}, {"data", "bm90IGEgcG5n"}, {"mimeType", "image/png"}}};
        for (int index = 0; index < 3; ++index)
            content.append(image);
        const QJsonObject item{{"id", "capture"},
                               {"type", "mcpToolCall"},
                               {"server", "ketplus_browser"},
                               {"tool", "browser_capture"},
                               {"status", "completed"},
                               {"result", QJsonObject{{"content", content}}}};
        const QJsonObject externalized = store.externalizeItem(item, "session-a");
        const QByteArray json = QJsonDocument(externalized).toJson(QJsonDocument::Compact);
        QVERIFY(!json.contains(base64.left(16).toLatin1()));
        QVERIFY(!json.contains("QVVESU9f"));
        QVERIFY(!containsKey(externalized, "data"));
        const QJsonArray blocks =
            externalized.value("result").toObject().value("content").toArray();
        QCOMPARE(blocks.size(), content.size());
        QCOMPARE(blocks.at(0), content.at(0));
        const QString hex =
            QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex());
        const QJsonObject reference = blocks.at(1).toObject();
        QCOMPARE(reference.value("media_ref").toString(), "sha256:" + hex);
        QCOMPARE(reference.value("mime").toString(), QStringLiteral("image/png"));
        QCOMPARE(reference.value("bytes").toInteger(), png.size());
        QCOMPARE(reference.value("width").toInt(), 640);
        QCOMPARE(reference.value("height").toInt(), 480);
        QCOMPARE(blocks.at(2).toObject(), reference);
        QCOMPARE(blocks.at(3).toObject().value("reason").toString(), "unsupported_media");
        QCOMPARE(blocks.at(4).toObject().value("reason").toString(), "unsupported_media");
        QVERIFY(blocks.at(5).toObject().contains("media_ref"));
        QVERIFY(blocks.at(6).toObject().contains("media_ref"));
        QCOMPARE(blocks.at(7).toObject().value("reason").toString(), "too_many_images");
        QCOMPARE(store.externalizeItem(QJsonObject{{"type", "agentMessage"}}, "session-a"),
                 QJsonObject({{"type", "agentMessage"}}));

        const QString file = QDir(mediaDirectory).filePath(hex + ".png");
        QVERIFY(QFileInfo::exists(file));
        QCOMPARE(QFileInfo(file).size(), png.size());
        QCOMPARE(QDir(mediaDirectory).entryList({"*.png"}, QDir::Files).size(), 1); // deduped
        const auto groupOrOther = QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                  QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                  QFileDevice::WriteOther | QFileDevice::ExeOther;
        QCOMPARE(QFile::permissions(file) & groupOrOther, QFileDevice::Permissions{});
        QCOMPARE(QFile::permissions(mediaDirectory) & groupOrOther, QFileDevice::Permissions{});
        QVERIFY(QFile::permissions(file).testFlag(QFileDevice::ReadOwner));

        // Session deletion prunes media no other session references.
        QVERIFY(store.externalizeItem(item, "session-b").contains("result"));
        store.releaseSession("session-a");
        QVERIFY(QFileInfo::exists(file));
        store.releaseSession("session-b");
        QVERIFY(!QFileInfo::exists(file));

        // LRU capacity: the store keeps only what fits, evicting the oldest write.
        McpMediaStore bounded(directory.filePath("bounded"), png.size());
        const QJsonObject first{{"result", QJsonObject{{"content", QJsonArray{image}}}}};
        const QByteArray other = fakePng(1, 1, 'b');
        const QJsonObject second{
            {"result", QJsonObject{{"content", QJsonArray{QJsonObject{
                                                   {"type", "image"},
                                                   {"data", QString::fromLatin1(other.toBase64())},
                                                   {"mimeType", "image/png"}}}}}}};
        (void)bounded.externalizeItem(first, "s");
        QTest::qWait(20);
        (void)bounded.externalizeItem(second, "s");
        const QStringList kept = QDir(bounded.directory()).entryList({"*.png"}, QDir::Files);
        QCOMPARE(kept.size(), 1);
        QVERIFY(kept.first().startsWith(QString::fromLatin1(
            QCryptographicHash::hash(other, QCryptographicHash::Sha256).toHex())));
        QCOMPARE(McpMediaStore::directoryForJournal(directory.filePath("state/journal.sqlite3")),
                 QDir(directory.filePath("state")).absoluteFilePath("media"));
    }

    void mcpMediaStoreHandlesResourceBlobsReferencedEvictionAndOrphanRefs() {
        QTemporaryDir directory;
        const QByteArray png = fakePng(4, 3, 'r');
        const QString base64 = QString::fromLatin1(png.toBase64());
        McpMediaStore store(directory.filePath("media"));
        const QJsonObject item{
            {"id", "resource"},
            {"type", "mcpToolCall"},
            {"result",
             QJsonObject{
                 {"content",
                  QJsonArray{
                      QJsonObject{{"type", "resource"},
                                  {"resource", QJsonObject{{"uri", "file:///shot.png"},
                                                           {"mimeType", "image/png"},
                                                           {"blob", base64}}}},
                      QJsonObject{{"type", "resource"},
                                  {"resource", QJsonObject{{"uri", "file:///secret.pdf"},
                                                           {"mimeType", "application/pdf"},
                                                           {"blob", "UERGX1NFQ1JFVF9CWVRFUw=="}}}},
                      QJsonObject{{"type", "resource"},
                                  {"resource",
                                   QJsonObject{{"uri", "file:///a.txt"}, {"text", "plain"}}}}}}}}};
        const QJsonObject externalized = store.externalizeItem(item, "session-r");
        const QByteArray json = QJsonDocument(externalized).toJson(QJsonDocument::Compact);
        QVERIFY(!json.contains(base64.left(16).toLatin1()));
        QVERIFY(!json.contains("UERGX1NF"));
        const QJsonArray blocks =
            externalized.value("result").toObject().value("content").toArray();
        QCOMPARE(blocks.at(0).toObject().value("type").toString(), "image");
        QCOMPARE(blocks.at(0).toObject().value("width").toInt(), 4);
        QVERIFY(blocks.at(0).toObject().value("media_ref").toString().startsWith("sha256:"));
        QCOMPARE(blocks.at(1).toObject().value("reason").toString(), "unsupported_media");
        QCOMPARE(blocks.at(2), item.value("result").toObject().value("content").toArray().at(2));

        // Capacity evicts unreferenced media before media a live session references.
        const QByteArray referencedPng = fakePng(8, 8, 'k');
        McpMediaStore bounded(directory.filePath("bounded"), referencedPng.size() + 8);
        (void)bounded.externalizeItem(
            {{"result",
              QJsonObject{{"content", QJsonArray{QJsonObject{
                                          {"type", "image"},
                                          {"data", QString::fromLatin1(referencedPng.toBase64())},
                                          {"mimeType", "image/png"}}}}}}},
            "live");
        const QString referencedHex = QString::fromLatin1(
            QCryptographicHash::hash(referencedPng, QCryptographicHash::Sha256).toHex());
        QTest::qWait(20);
        const QString orphan = QDir(bounded.directory()).filePath(QString(64, 'e') + ".png");
        QFile orphanFile(orphan);
        QVERIFY(orphanFile.open(QIODevice::WriteOnly));
        orphanFile.write(fakePng(9, 9, 'o'));
        orphanFile.close();
        bounded.enforceCapacity();
        QVERIFY(!QFileInfo::exists(orphan));
        QVERIFY(QFileInfo::exists(QDir(bounded.directory()).filePath(referencedHex + ".png")));

        // Startup reconciliation releases refs of sessions the journal no longer has.
        bounded.retainSessions({"live"});
        QVERIFY(QFileInfo::exists(QDir(bounded.directory()).filePath(referencedHex + ".png")));
        bounded.retainSessions({"another"});
        QVERIFY(!QFileInfo::exists(QDir(bounded.directory()).filePath(referencedHex + ".png")));
    }

    void cloudArgumentSummaryDropsFreeTextAndUrlSecrets() {
        const QString summary = cloudArgumentSummary(
            {{"url", "https://app.example.com/callback?code=OAUTH_CODE_SECRET#access_token=FRAG"},
             {"text", "TYPED_PASSWORD_VALUE"},
             {"value", "TYPED_VALUE"},
             {"script", "document.cookie"},
             {"body", "REQUEST_BODY"},
             {"headers", "Authorization: Bearer HEADER_SECRET"},
             {"apiKey", "KEY_SECRET"},
             {"selector", "#login button"},
             {"note", "see https://u:p@example.com/x?token=QUERY_SECRET and token=INLINE_SECRET"},
             {"fullPage", true}});
        for (const auto* secret :
             {"OAUTH_CODE_SECRET", "FRAG", "TYPED_PASSWORD_VALUE", "TYPED_VALUE", "document.cookie",
              "REQUEST_BODY", "HEADER_SECRET", "KEY_SECRET", "QUERY_SECRET", "INLINE_SECRET",
              "u:p@"}) {
            QVERIFY2(!summary.contains(QLatin1String(secret)), secret);
        }
        QVERIFY2(summary.contains("url=https://app.example.com/callback"), qPrintable(summary));
        QVERIFY(summary.contains("selector=#login button"));
        QCOMPARE(sanitizeCloudUrl("http://[::1]:3000/a?b=c#d"),
                 QStringLiteral("http://[::1]:3000/a"));
        // Caps never split a surrogate pair.
        const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
        const QString cappedText = capCloudText(QString(3, 'a') + emoji + QString(10, 'b'), 5);
        QVERIFY(!cappedText.at(cappedText.size() - 2).isHighSurrogate());
        for (qsizetype index = 0; index < cappedText.size(); ++index) {
            if (cappedText.at(index).isHighSurrogate())
                QVERIFY(index + 1 < cappedText.size() && cappedText.at(index + 1).isLowSurrogate());
        }
    }

    void cloudMcpToolCallProjectionDropsMediaAndLongText() {
        const QByteArray png = fakePng(2, 2, 'c');
        const QString base64 = QString::fromLatin1(png.toBase64());
        const QString longText(4096, QLatin1Char('x'));
        for (const bool externalizedFirst : {false, true}) {
            QJsonObject item{
                {"id", "call-1"},
                {"type", "mcpToolCall"},
                {"server", "ketplus_browser"},
                {"tool", "browser_capture"},
                {"status", "completed"},
                {"arguments", QJsonObject{{"url", "http://localhost:5173/"},
                                          {"sessionId", "s-secret-id"},
                                          {"authToken", "TOKEN_SHOULD_NOT_REACH_CLOUD"},
                                          {"fullPage", true}}},
                {"result",
                 QJsonObject{
                     {"isError", false},
                     {"structuredContent", QJsonObject{{"data", base64}}},
                     {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", longText}},
                                            QJsonObject{{"type", "image"},
                                                        {"data", base64},
                                                        {"mimeType", "image/png"}}}}}}};
            QTemporaryDir directory;
            if (externalizedFirst)
                item = McpMediaStore(directory.filePath("media")).externalizeItem(item, "s");
            const QJsonObject projected = projectEventPayloadForCloud(
                "item.completed",
                {{"item_id", "call-1"}, {"item_type", "mcpToolCall"}, {"item", item}});
            const QByteArray bytes = QJsonDocument(projected).toJson(QJsonDocument::Compact);
            QVERIFY(!containsKey(projected, "data"));
            QVERIFY(!containsKey(projected, "structuredContent"));
            QVERIFY(!bytes.contains(base64.left(16).toLatin1()));
            QVERIFY(!bytes.contains("TOKEN_SHOULD_NOT_REACH_CLOUD"));
            QVERIFY(!bytes.contains("s-secret-id"));
            QVERIFY(!bytes.contains(QByteArray(300, 'x')));
            const QJsonObject projectedItem = projected.value("item").toObject();
            QCOMPARE(projectedItem.value("tool").toString(), "browser_capture");
            QCOMPARE(projectedItem.value("server").toString(), "ketplus_browser");
            QCOMPARE(projectedItem.value("status").toString(), "completed");
            QVERIFY(projectedItem.value("arguments_summary")
                        .toString()
                        .contains("url=http://localhost:5173/"));
            const QJsonArray blocks =
                projectedItem.value("result").toObject().value("content").toArray();
            QCOMPARE(blocks.size(), 2);
            QVERIFY(blocks.at(0).toObject().value("truncated").toBool());
            QCOMPARE(blocks.at(1).toObject().contains("media_ref"), externalizedFirst);
        }
    }
    void cloudProjectionSaysWhatACommandDoesAndWhichFileAToolUsed() {
        const QJsonObject command = projectEventPayloadForCloud(
            QStringLiteral("item.started"),
            {{"item_id", "bash-1"},
             {"item_type", "commandExecution"},
             {"item", QJsonObject{{"id", "bash-1"},
                                  {"type", "commandExecution"},
                                  {"command", "git status"},
                                  {"description", "Show status token=abc123 " + QString(300, 'd')},
                                  {"aggregatedOutput", "private"}}}});
        const QString description =
            command.value("item").toObject().value("description").toString();
        QVERIFY(description.startsWith("Show status token=[redacted]"));
        QVERIFY(description.size() <= 160);
        QVERIFY(!command.value("item").toObject().contains("aggregatedOutput"));

        const auto tool = [](const QString& path) {
            return projectEventPayloadForCloud(
                       QStringLiteral("item.completed"),
                       {{"item_id", "read-1"},
                        {"item_type", "dynamicToolCall"},
                        {"item", QJsonObject{{"id", "read-1"},
                                             {"type", "dynamicToolCall"},
                                             {"tool", "Read"},
                                             {"path", path},
                                             {"arguments", QJsonObject{{"file_path", "/w/" + path}}}}}})
                .value("item")
                .toObject();
        };
        QCOMPARE(tool("apps/ios/App.swift").value("path").toString(), "apps/ios/App.swift");
        QCOMPARE(tool("/etc/hosts").value("path").toString(), "/etc/hosts");
        QVERIFY(!tool("../other/file").contains("path"));
        QVERIFY(!tool("a/../../b").contains("path"));
        QVERIFY(!tool("apps/ios/App.swift").contains("outsideWorktree"));
        const QJsonObject outside =
            projectEventPayloadForCloud(
                QStringLiteral("item.completed"),
                {{"item_type", "dynamicToolCall"},
                 {"item", QJsonObject{{"id", "read-2"},
                                      {"type", "dynamicToolCall"},
                                      {"tool", "Read"},
                                      {"outsideWorktree", true},
                                      {"arguments", QJsonObject{{"file_path", "/etc/hosts"}}}}}})
                .value("item")
                .toObject();
        QVERIFY(!outside.contains("path"));
        QCOMPARE(outside.value("outsideWorktree").toBool(), true);
    }

    void cloudCommandProjectionKeepsCommandAndStripsOutput() {
        const QString command = QStringLiteral("/bin/zsh -lc \"sed -n '1,200p' source.cpp\"");
        const QJsonObject payload{
            {"item_id", "command-1"},
            {"item_type", "commandExecution"},
            {"command", command},
            {"item",
             QJsonObject{{"id", "command-1"},
                         {"type", "commandExecution"},
                         {"command", command},
                         {"status", "completed"},
                         {"aggregatedOutput", "private file contents"},
                         {"commandActions", QJsonArray{QJsonObject{{"command", command}}}}}},
        };

        const QJsonObject projected =
            projectEventPayloadForCloud(QStringLiteral("item.completed"), payload);
        QCOMPARE(projected.value("command").toString(), command);
        const QJsonObject item = projected.value("item").toObject();
        QCOMPARE(item.value("command").toString(), command);
        QCOMPARE(item.value("status").toString(), QStringLiteral("completed"));
        QVERIFY(!item.contains("aggregatedOutput"));
        QVERIFY(!item.contains("commandActions"));

        const QJsonObject delta = projectEventPayloadForCloud(
            QStringLiteral("command.output_delta"),
            {{"item_id", "command-1"}, {"item_type", "commandExecution"}, {"delta", "secret"}});
        QCOMPARE(delta.value("item_id").toString(), QStringLiteral("command-1"));
        QVERIFY(!delta.contains("delta"));

        const QStringList variants{
            QStringLiteral("command.output_aggregated"), QStringLiteral("command.stdout.delta"),
            QStringLiteral("command.stderr"), QStringLiteral("item/commandExecution/outputDelta"),
            QStringLiteral("future-command-output-stream")};
        for (const QString& eventType : variants) {
            const QJsonObject unknown =
                projectEventPayloadForCloud(eventType, {{"item_id", "command-1"},
                                                        {"item_type", "commandExecution"},
                                                        {"command", command},
                                                        {"status", "completed"},
                                                        {"output", "secret-output"},
                                                        {"stdout", "secret-stdout"},
                                                        {"stderr", "secret-stderr"},
                                                        {"body", "secret-body"},
                                                        {"data", "secret-data"},
                                                        {"delta", "secret-delta"}});
            QCOMPARE(unknown.value("command").toString(), command);
            QCOMPARE(unknown.value("status").toString(), QStringLiteral("completed"));
            QVERIFY(!QJsonDocument(unknown).toJson(QJsonDocument::Compact).contains("secret"));
        }

        const QJsonObject assistant = projectEventPayloadForCloud(
            QStringLiteral("assistant.delta"),
            {{"item_type", "agentMessage"}, {"delta", "legitimate assistant text"}});
        QCOMPARE(assistant.value("delta").toString(), QStringLiteral("legitimate assistant text"));
    }

    void providerNeutralCommandOutputPrivacyConformanceFixture() {
        const QString path = QFINDTESTDATA("../fixtures/command-output-privacy-v1.json");
        QVERIFY2(!path.isEmpty(), "privacy fixture was not found");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject fixture = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(fixture.value("version").toInt(), 1);
        for (const QJsonValue& value : fixture.value("cases").toArray()) {
            const QJsonObject scenario = value.toObject();
            QCOMPARE(projectEventPayloadForCloud(scenario.value("eventType").toString(),
                                                 scenario.value("payload").toObject()),
                     scenario.value("expected").toObject());
        }
    }

    void validatesRequiredEnvelopeFields() {
        const QJsonObject valid{{"protocol_version", 0},
                                {"message_id", "message-1"},
                                {"message_type", "session.list"},
                                {"sent_at", "2026-09-10T00:00:00.000Z"},
                                {"payload", QJsonObject{}}};
        QVERIFY(protocol::validateEnvelope(valid).valid);

        QJsonObject changed = valid;
        changed.insert("protocol_version", 1);
        QCOMPARE(protocol::validateEnvelope(changed).errorCode, "unsupported_protocol");
        changed = valid;
        changed.remove("message_id");
        QCOMPARE(protocol::validateEnvelope(changed).errorCode, "invalid_message_id");
        changed = valid;
        changed.insert("payload", QJsonArray{});
        QCOMPARE(protocol::validateEnvelope(changed).errorCode, "invalid_payload");
    }

    void encodesBigEndianLengthPrefix() {
        const QJsonObject envelope{{"protocol_version", 0},
                                   {"message_id", "message-1"},
                                   {"message_type", "session.list"},
                                   {"sent_at", "now"},
                                   {"payload", QJsonObject{}}};
        const QByteArray frame = protocol::encodeFrame(envelope);
        QVERIFY(frame.size() > 4);
        const quint32 size = (static_cast<quint32>(static_cast<uchar>(frame[0])) << 24U) |
                             (static_cast<quint32>(static_cast<uchar>(frame[1])) << 16U) |
                             (static_cast<quint32>(static_cast<uchar>(frame[2])) << 8U) |
                             static_cast<quint32>(static_cast<uchar>(frame[3]));
        QCOMPARE(size, static_cast<quint32>(frame.size() - 4));
        QCOMPARE(QJsonDocument::fromJson(frame.mid(4)).object(), envelope);
    }

    void journalPersistsOrderedReplayAndDeduplication() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("journal.sqlite3");
        {
            LocalJournal journal(path);
            QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
            StoredSession session{"session-1", directory.path(), "codex", {}, {}, "ready", 3, 0, 0,
                                  {}};
            QVERIFY2(journal.upsertSession(session), qPrintable(journal.errorString()));
            QString appendError;
            QVERIFY2(journal.appendEvent("session-1", 3, "first", {{"value", 1}}, &appendError) ==
                         1,
                     qPrintable(appendError));
            QCOMPARE(journal.appendEvent("session-1", 3, "second", {{"value", 2}}), 2);
            const QJsonObject command{{"ok", true}, {"accepted", true}};
            QVERIFY(journal.storeCommandResult("command-1", command));
            QVERIFY(journal.storeCommandResult("command-1", {{"ok", false}}));
            QCOMPARE(journal.commandResult("command-1"), command);
            const QJsonObject cloudEvent{{"sessionId", "session-1"},
                                         {"sessionGeneration", 3},
                                         {"cursor", 1},
                                         {"content", QJsonObject{{"ciphertext", "opaque"}}}};
            QVERIFY(journal.storeCloudEvent("session-1", 3, 1, cloudEvent));
            QVERIFY(journal.storeCloudEvent("session-1", 3, 1, cloudEvent));
            QVERIFY(!journal.storeCloudEvent("session-1", 3, 1, QJsonObject{{"cursor", 1}}));
            QCOMPARE(journal.cloudEvent("session-1", 3, 1), cloudEvent);
        }
        LocalJournal reopened(path);
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        const auto sessions = reopened.sessions();
        QCOMPARE(sessions.size(), 1);
        QCOMPARE(sessions.first().lastCursor, 2);
        const QJsonArray suffix = reopened.eventsAfter("session-1", 3, 1);
        QCOMPARE(suffix.size(), 1);
        QCOMPARE(suffix.first().toObject().value("cursor").toInteger(), 2);
        QCOMPARE(suffix.first().toObject().value("event_type").toString(), "second");
        const QJsonArray sent{reopened.cloudEvent("session-1", 3, 1)};
        QVERIFY(reopened.prepareCloudEventUpload(sent));
        QVERIFY(reopened.acknowledgeCloudEventUpload("fixture-scope", "session-1", 3, sent, 1));
        QCOMPARE(reopened.cloudUploadCursor("fixture-scope", "session-1", 3), 1);
        QVERIFY(reopened.pruneAcknowledgedCloudEvents());
        QVERIFY(reopened.cloudEvent("session-1", 3, 1).isEmpty());
        QVERIFY(reopened.acknowledgeCursor("cloud-control-v2", "session-1", 3, 0));
        QVERIFY(reopened.acknowledgeCursor("desktop", "session-1", 3, 2));
        QVERIFY(reopened.acknowledgeCursor("desktop", "session-1", 3, 1));
        QCOMPARE(reopened.eventsAfter("session-1", 3, 0).size(), 2);
        QVERIFY(reopened.acknowledgeCursor("cloud-control-v2", "session-1", 3, 2));
        QVERIFY(reopened.eventsAfter("session-1", 3, 0).isEmpty());
        QCOMPARE(reopened.earliestCursor("session-1", 3), 3);
        QCOMPARE(reopened.earliestCompactedCursor("session-1", 3), 1);
        const EventReplayPage compacted = reopened.compactedEventsAfter("session-1", 3, 0, 500);
        QCOMPARE(compacted.events.size(), 2);
        QCOMPARE(compacted.nextCursor, 2);
    }

    // A restart ends the in-flight turn with turn.completed, once, so clients stop showing it
    // as running.
    void restartClosesTheInterruptedTurnOnce() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        const auto store = [&](const QString& id, const QString& state,
                               const QStringList& turnEvents) {
            StoredSession stored;
            stored.id = id;
            stored.worktreePath = QFileInfo(directory.path()).canonicalFilePath();
            stored.adapter = QStringLiteral("codex");
            stored.state = state;
            QVERIFY(journal.upsertSession(stored));
            for (const QString& type : turnEvents)
                QVERIFY(journal.appendEvent(id, 1, type, {}) > 0);
        };
        store(QStringLiteral("in-flight"), QStringLiteral("running"),
              {QStringLiteral("turn.started")});
        store(QStringLiteral("finished"), QStringLiteral("running"),
              {QStringLiteral("turn.started"), QStringLiteral("turn.completed")});
        const auto events = [&](const QString& id) {
            return journal.compactedEventsAfter(id, 1, 0, 500).events;
        };
        CodexAdapter adapter(QStringLiteral("/nonexistent/ketplus-test-codex"));
        for (int restart = 0; restart < 2; ++restart)
            SessionHost host(&journal, &adapter);

        const QJsonArray inFlight = events(QStringLiteral("in-flight"));
        QCOMPARE(inFlight.size(), 2);
        const QJsonObject closing = inFlight.last().toObject();
        QCOMPARE(closing.value(QStringLiteral("event_type")).toString(),
                 QStringLiteral("turn.completed"));
        QCOMPARE(closing.value(QStringLiteral("payload"))
                     .toObject()
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("interrupted"));
        QCOMPARE(events(QStringLiteral("finished")).size(), 2);
        for (const StoredSession& session : journal.sessions())
            QCOMPARE(session.state, QStringLiteral("interrupted"));
    }

    void journalRetainsAnEnrolledWorktreeAfterItsLastConversationIsDeleted() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString worktree = directory.filePath(QStringLiteral("worktree"));
        QVERIFY(QDir().mkpath(worktree));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.upsertSession({QStringLiteral("only-session"),
                                        worktree,
                                        QStringLiteral("codex"),
                                        {},
                                        {},
                                        QStringLiteral("ready"),
                                        1,
                                        0,
                                        0,
                                        {}}),
                 qPrintable(journal.errorString()));
        QCOMPARE(journal.worktreePaths(), QStringList{worktree});

        QString error;
        QVERIFY2(journal.deleteSession(QStringLiteral("only-session"), 1, &error),
                 qPrintable(error));
        QVERIFY(journal.sessions().isEmpty());
        QCOMPARE(journal.worktreePaths(), QStringList{worktree});
    }

    // A deleted worktree's conversations stay under the repository it belonged to, whether
    // that was recorded while it existed, is still listed in the repository's Git metadata,
    // or only its <repository>-<branch> name is left beside a checkout, including one saved
    // before the catalog existed.
    void deletedWorktreesKeepTheRepositoryTheyBelongedTo() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDir root(QFileInfo(directory.path()).canonicalFilePath());
        const QString repository = root.filePath(QStringLiteral("repository"));
        const auto link = [&](const QString& name, bool checkout) {
            const QString admin = repository + QStringLiteral("/.git/worktrees/") + name;
            QVERIFY(QDir().mkpath(admin));
            QFile gitdir(admin + QStringLiteral("/gitdir"));
            QVERIFY(gitdir.open(QIODevice::WriteOnly));
            gitdir.write((root.filePath(name) + QStringLiteral("/.git\n")).toUtf8());
            if (!checkout)
                return;
            QVERIFY(QDir().mkpath(root.filePath(name)));
            QFile marker(root.filePath(name + QStringLiteral("/.git")));
            QVERIFY(marker.open(QIODevice::WriteOnly));
            marker.write(("gitdir: " + admin + "\n").toUtf8());
        };
        QVERIFY(QDir().mkpath(repository + QStringLiteral("/.git")));
        QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("other/.git"))));
        link(QStringLiteral("feature"), true);
        link(QStringLiteral("pruned-later"), false);
        const QString journalPath = root.filePath(QStringLiteral("journal.sqlite3"));
        {
            LocalJournal journal(journalPath);
            for (const QString& name :
                 {QStringLiteral("repository"), QStringLiteral("feature"),
                  QStringLiteral("pruned-later"), QStringLiteral("repository-long-gone"),
                  QStringLiteral("other-before-catalog"), QStringLiteral("unknown")}) {
                StoredSession session;
                session.id = name;
                session.worktreePath = root.filePath(name);
                session.adapter = QStringLiteral("codex");
                session.state = QStringLiteral("ready");
                QVERIFY2(journal.upsertSession(session), qPrintable(journal.errorString()));
            }
            const QHash<QString, QString> resolved = resolveWorktreeRepositories(journal);
            QCOMPARE(resolved.value(root.filePath(QStringLiteral("feature"))), repository);
            QCOMPARE(resolved.value(root.filePath(QStringLiteral("pruned-later"))), repository);
            QCOMPARE(resolved.value(root.filePath(QStringLiteral("repository-long-gone"))),
                     repository);
            QVERIFY(resolved.value(root.filePath(QStringLiteral("unknown"))).isEmpty());
            QVERIFY(resolved.value(repository).isEmpty());
        }
        QVERIFY(QDir(root.filePath(QStringLiteral("feature"))).removeRecursively());
        QVERIFY(QDir(repository + QStringLiteral("/.git/worktrees")).removeRecursively());
        const QString connection = QStringLiteral("worktree-catalog-test");
        {
            QSqlDatabase database =
                QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(journalPath);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral(
                "DELETE FROM worktree_catalog WHERE path LIKE '%other-before-catalog'")));
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        LocalJournal reopened(journalPath);
        const QHash<QString, QString> resolved = resolveWorktreeRepositories(reopened);
        QCOMPARE(resolved.value(root.filePath(QStringLiteral("feature"))), repository);
        QCOMPARE(resolved.value(root.filePath(QStringLiteral("pruned-later"))), repository);
        QCOMPARE(resolved.value(root.filePath(QStringLiteral("other-before-catalog"))),
                 root.filePath(QStringLiteral("other")));
        QVERIFY(resolved.value(root.filePath(QStringLiteral("unknown"))).isEmpty());
    }

    // A phone starting a conversation sees every checkout of the repository and its local and
    // remote-tracking branches, and can cut a new branch into a worktree beside the main
    // checkout the way the desktop's dialog does, never over an existing branch or folder.
    void phoneCanListBranchesAndCutANewWorktree() {
        if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
            QSKIP("Git is not installed.");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDir root(QFileInfo(directory.path()).canonicalFilePath());
        const QString repository = root.filePath(QStringLiteral("app"));
        const auto git = [](const QString& in, const QStringList& arguments) {
            QProcess process;
            process.setWorkingDirectory(in);
            process.start(QStringLiteral("git"),
                          QStringList{QStringLiteral("-c"), QStringLiteral("user.name=Test"),
                                      QStringLiteral("-c"),
                                      QStringLiteral("user.email=test@example.invalid")} +
                              arguments);
            return process.waitForFinished(10000) && process.exitCode() == 0;
        };
        QVERIFY(QDir().mkpath(repository));
        QVERIFY(git(repository, {"init", "-q", "-b", "develop"}));
        QVERIFY(git(repository, {"commit", "-q", "--allow-empty", "-m", "first"}));
        QVERIFY(git(repository, {"branch", "main"}));
        QVERIFY(git(repository, {"update-ref", "refs/remotes/origin/release/1.0", "HEAD"}));
        QVERIFY(git(repository,
                    {"symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/release/1.0"}));

        const RepositoryBranches branches = readRepositoryBranches(repository);
        QVERIFY(branches.ok);
        QCOMPARE(branches.local, (QStringList{"develop", "main"}));
        QCOMPARE(branches.remote, QStringList{"origin/release/1.0"});

        const AddedWorktree added =
            addWorktree(repository, QStringLiteral("feat/phone"), QStringLiteral("origin/release/1.0"));
        QVERIFY2(added.errorCode.isEmpty(), qPrintable(added.message));
        QCOMPARE(added.path, root.filePath(QStringLiteral("app-feat-phone")));
        QCOMPARE(checkoutBranch(added.path), QStringLiteral("feat/phone"));
        QCOMPARE(worktreeRepository(added.path), repository);
        QCOMPARE(repositoryCheckouts(repository), (QStringList{repository, added.path}));

        QCOMPARE(addWorktree(repository, "main", "develop").errorCode, QStringLiteral("branch_exists"));
        QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("app-taken"))));
        QCOMPARE(addWorktree(repository, "taken", "develop").errorCode, QStringLiteral("worktree_exists"));
        QCOMPARE(addWorktree(repository, "fresh", "HEAD~1").errorCode,
                 QStringLiteral("base_branch_not_found"));
        QCOMPARE(addWorktree(repository, "fresh", "--orphan").errorCode,
                 QStringLiteral("base_branch_not_found"));
        for (const QString& invalid : {"", "has space", "-x", "a..b", "a/", "a.lock", "x/.y", "a:b", "@"})
            QVERIFY2(!validNewBranchName(invalid), qPrintable(invalid));
        QVERIFY(validNewBranchName(QStringLiteral("feat/mobile-ui_2.0")));
        QCOMPARE(addWorktree(repository, "bad name", "develop").errorCode,
                 QStringLiteral("invalid_branch_name"));
        QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("app-fresh"))));
    }

    void appendRequiresEveryDurableWrite_data() {
        QTest::addColumn<QString>("fault");
        QTest::addColumn<bool>("large");
        for (bool large : {false, true})
            for (const auto& fault :
                 {QString("raw"), QString("replay"), QString("state-insert"),
                  QString("state-update"), QString("session"), QString("commit"),
                  QString("revision"), QString("activity-head-insert"),
                  QString("activity-head-update"), QString("activity-item-insert"),
                  QString("activity-item-update"), QString("activity-summary-insert"),
                  QString("activity-summary-update")})
                QTest::newRow(qPrintable((large ? "large-" : "small-") + fault)) << fault << large;
        QTest::newRow("large-manifest") << "manifest" << true;
        QTest::newRow("large-fragment") << "fragment" << true;
        QTest::newRow("large-fragment-late") << "fragment-late" << true;
        QTest::newRow("large-raw-late") << "raw-late" << true;
        QTest::newRow("large-state-chunk") << "state-chunk" << true;
    }
    void appendRequiresEveryDurableWrite() {
        QFETCH(QString, fault);
        QFETCH(bool, large);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(journal.upsertSession({"s", dir.path(), "codex", {}, {}, "ready", 1, 0, 0, {}}));
        QCOMPARE(journal.appendEvent("s", 1, "user.message", {{"text", "before"}}), 1);
        if (fault == "revision")
            QVERIFY(journal.appendEvent("s", 1, "assistant.delta",
                                        {{"item_id", "probe"}, {"delta", "old"}}) > 1);
        if (fault.startsWith("activity-"))
            QVERIFY(journal.appendEvent("s", 1, "item.started",
                                        {{"item_id", "probe"},
                                         {"item_type", "mcpToolCall"},
                                         {"item", QJsonObject{{"id", "probe"},
                                                              {"type", "mcpToolCall"},
                                                              {"tool", "read_file"}}}}) > 1);
        const auto rawBefore = journal.eventsAfter("s", 1, 0);
        const auto replayBefore = journal.compactedEventsAfter("s", 1, 0, 500).events;
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        const auto snapshot = [&] {
            QVariantMap result;
            for (const auto& table : {"events", "replay_events", "replay_projection_state",
                                      "large_event_payloads", "large_event_payload_chunks",
                                      "activity_heads", "activity_items", "activity_summaries"}) {
                QSqlQuery q(db);
                if (!q.exec(QString("SELECT count(*) FROM %1").arg(table)) || !q.next()) {
                    QTest::qFail(qPrintable(q.lastError().text()), __FILE__, __LINE__);
                    return QVariantMap{};
                }
                result.insert(table, q.value(0));
            }
            for (const auto& table : {"replay_projection_state", "activity_heads", "activity_items",
                                      "activity_summaries"}) {
                QSqlQuery rows(db);
                if (!rows.exec(QString("SELECT * FROM %1 ORDER BY 1,2,3").arg(table))) {
                    QTest::qFail(qPrintable(rows.lastError().text()), __FILE__, __LINE__);
                    return QVariantMap{};
                }
                QVariantList values;
                while (rows.next())
                    for (int i = 0; i < rows.record().count(); ++i)
                        values.append(rows.value(i));
                result.insert(QString(table) + "-values", values);
            }
            QSqlQuery q(db);
            if (!q.exec("SELECT last_cursor,current_turn_sequence FROM sessions WHERE id='s'") ||
                !q.next()) {
                QTest::qFail(qPrintable(q.lastError().text()), __FILE__, __LINE__);
                return QVariantMap{};
            }
            result.insert("cursor", q.value(0));
            result.insert("turn", q.value(1));
            return result;
        };
        const auto before = snapshot();
        QSqlQuery q(db);
        if (fault == "commit") {
            QVERIFY(q.exec("CREATE TABLE append_parent(id INTEGER PRIMARY KEY)"));
            QVERIFY(q.exec("CREATE TABLE append_child(id INTEGER REFERENCES append_parent(id) "
                           "DEFERRABLE INITIALLY DEFERRED)"));
            QVERIFY(q.exec("CREATE TRIGGER fail_append AFTER UPDATE ON sessions "
                           "WHEN NEW.last_cursor>OLD.last_cursor BEGIN INSERT INTO append_child "
                           "VALUES(1); END"));
        } else {
            QString action = "INSERT";
            QString condition;
            QString table = fault.startsWith("raw")        ? "events"
                            : fault == "replay"            ? "replay_events"
                            : fault == "session"           ? "sessions"
                            : fault == "manifest"          ? "large_event_payloads"
                            : fault.startsWith("fragment") ? "large_event_payload_chunks"
                                                           : "replay_projection_state";
            if (fault.startsWith("activity-head"))
                table = "activity_heads";
            if (fault.startsWith("activity-item"))
                table = "activity_items";
            if (fault.startsWith("activity-summary"))
                table = "activity_summaries";
            if (fault == "session" || fault.endsWith("update") || fault == "revision")
                action = "UPDATE";
            if (fault == "revision")
                condition = "WHEN NEW.revision > OLD.revision";
            if (fault == "fragment-late")
                condition = "WHEN NEW.chunk_offset > 0";
            if (fault == "raw-late")
                condition = "WHEN NEW.cursor > 2";
            if (fault == "state-chunk")
                condition = "WHEN NEW.last_cursor = 3";
            QVERIFY(q.exec(QString("CREATE TRIGGER fail_append BEFORE %1 ON %2 %3 "
                                   "BEGIN SELECT RAISE(IGNORE); END")
                               .arg(action, table, condition)));
        }
        const bool item = large || fault == "revision" || fault.startsWith("activity-");
        const QString type = item ? "item.completed" : "assistant.delta";
        const QString itemType = fault.startsWith("activity-") ? "mcpToolCall" : "agentMessage";
        const QJsonObject payload =
            item ? QJsonObject{{"item_id", "probe"},
                               {"item_type", itemType},
                               {"item",
                                QJsonObject{
                                    {"id", "probe"},
                                    {"type", itemType},
                                    {"tool", "read_file"},
                                    {"text",
                                     QString(large ? 2 * protocol::maximumEventPayloadBytes : 32,
                                             'x')}}}}
                 : QJsonObject{{"item_id", "probe"}, {"delta", "after"}};
        QString error;
        QCOMPARE(journal.appendEvent("s", 1, type, payload, &error), -1);
        QVERIFY(!error.isEmpty());
        QVERIFY2(q.exec("BEGIN"), qPrintable(q.lastError().text()));
        QVERIFY(q.exec("ROLLBACK"));
        QCOMPARE(snapshot(), before);
        QCOMPARE(journal.eventsAfter("s", 1, 0), rawBefore);
        QCOMPARE(journal.compactedEventsAfter("s", 1, 0, 500).events, replayBefore);
        QVERIFY(q.exec("DROP TRIGGER fail_append"));
        const auto accepted = journal.appendEvent("s", 1, type, payload, &error);
        QVERIFY2(accepted > 1, qPrintable(error));
        QCOMPARE(journal.sessionHead("s").cursor, accepted);
        LocalJournal reopened(path);
        QVERIFY(reopened.isOpen());
        QCOMPARE(reopened.sessionHead("s").cursor, accepted);
        QCOMPARE(reopened.eventsAfter("s", 1, 0), journal.eventsAfter("s", 1, 0));
        QCOMPARE(reopened.compactedEventsAfter("s", 1, 0, 500).events,
                 journal.compactedEventsAfter("s", 1, 0, 500).events);
    }

    void journalRejectsOversizedPayloadBeforePersistence() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY2(journal.upsertSession(
                     {"session-1", directory.path(), "codex", {}, {}, "ready", 1, 0, 0, {}}),
                 qPrintable(journal.errorString()));
        QString failure;
        const QString oversized(protocol::maximumEventPayloadBytes + 1, QLatin1Char('x'));
        QCOMPARE(journal.appendEvent("session-1", 1, "oversized", {{"data", oversized}}, &failure),
                 -1);
        QVERIFY(!failure.isEmpty());
        QVERIFY(journal.eventsAfter("session-1", 1, 0).isEmpty());
    }

    void oversizedCommandCannotBypassCloudOutputStrippingThroughChunks() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(journal.upsertSession(
            {"session-1", directory.path(), "codex", {}, {}, "ready", 1, 0, 0, {}}));
        const QJsonObject command{
            {"item_id", "command"},
            {"item_type", "commandExecution"},
            {"item", QJsonObject{{"id", "command"},
                                 {"type", "commandExecution"},
                                 {"command", "safe command"},
                                 {"status", "completed"},
                                 {"aggregatedOutput", QString(300 * 1024, QLatin1Char('s'))}}}};
        QString error;
        QCOMPARE(journal.appendEvent("session-1", 1, "item.completed", command, &error), -1);
        QVERIFY(!error.isEmpty());
        const QJsonArray raw = journal.eventsAfter("session-1", 1, 0);
        QVERIFY(raw.isEmpty());
        const QJsonObject projected = projectEventPayloadForCloud("item.completed", command);
        QVERIFY(
            !QJsonDocument(projected).toJson(QJsonDocument::Compact).contains("aggregatedOutput"));
        QVERIFY(!projected.value("item").toObject().contains("aggregatedOutput"));
    }

    void compactedReplaySkipsCompletedDeltasAndRetainsPartialOutput() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY2(journal.upsertSession(
                     {"session-1", directory.path(), "codex", {}, {}, "ready", 1, 0, 0, {}}),
                 qPrintable(journal.errorString()));

        QCOMPARE(journal.appendEvent("session-1", 1, "assistant.delta",
                                     {{"item_id", "completed"}, {"delta", "partial"}}),
                 1);
        QCOMPARE(journal.appendEvent("session-1", 1, "item.completed",
                                     {{"item", QJsonObject{{"id", "completed"},
                                                           {"type", "agentMessage"},
                                                           {"text", "final"}}}}),
                 2);
        QCOMPARE(journal.appendEvent("session-1", 1, "command.output_delta",
                                     {{"item_id", "command"}, {"delta", "ignored"}}),
                 3);
        QCOMPARE(journal.appendEvent("session-1", 1, "assistant.delta",
                                     {{"item_id", "running"}, {"delta", "keep"}}),
                 4);

        const EventReplayPage first = journal.compactedEventsAfter("session-1", 1, 0, 1);
        QCOMPARE(first.events.size(), 1);
        QCOMPARE(first.events.first().toObject().value("event_type").toString(), "item.completed");
        QCOMPARE(first.nextCursor, 2);

        const EventReplayPage second =
            journal.compactedEventsAfter("session-1", 1, first.nextCursor, 2);
        QCOMPARE(second.events.size(), 2);
        QCOMPARE(second.events.last().toObject().value("event_type").toString(), "assistant.delta");
        QCOMPARE(
            second.events.last().toObject().value("payload").toObject().value("delta").toString(),
            "keep");
        QCOMPARE(second.nextCursor, 4);
        QCOMPARE(second.events.first().toObject().value("event_type"), "command.output_delta");
    }

    void transcriptPagesNewestThreeTurnsAndLoadsOlderTurnsBackward() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY2(journal.upsertSession(
                     {"session-1", directory.path(), "codex", {}, {}, "ready", 1, 0, 0, {}}),
                 qPrintable(journal.errorString()));

        for (int turn = 1; turn <= 5; ++turn) {
            QCOMPARE(journal.appendEvent("session-1", 1, "user.message",
                                         {{"text", QStringLiteral("prompt-%1").arg(turn)}}),
                     (turn - 1) * 3 + 1);
            QCOMPARE(journal.appendEvent(
                         "session-1", 1, "item.completed",
                         {{"item", QJsonObject{{"id", QStringLiteral("answer-%1").arg(turn)},
                                               {"type", "agentMessage"},
                                               {"text", QStringLiteral("answer-%1").arg(turn)}}}}),
                     (turn - 1) * 3 + 2);
            QCOMPARE(
                journal.appendEvent("session-1", 1, "turn.completed", {{"status", "completed"}}),
                turn * 3);
        }

        const TranscriptPage latest = journal.transcriptPage("session-1", 1, 0, 3);
        QCOMPARE(latest.snapshotCursor, 15);
        QCOMPARE(latest.earliestTurnSequence, 3);
        QVERIFY(latest.hasMoreBefore);
        QVERIFY(!latest.truncated);
        QCOMPARE(latest.events.size(), 9);
        QCOMPARE(latest.events.first().toObject().value("turn_sequence").toInteger(), 3);
        QCOMPARE(latest.events.last().toObject().value("turn_sequence").toInteger(), 5);

        const TranscriptPage older = journal.transcriptPage("session-1", 1, 3, 3);
        QCOMPARE(older.earliestTurnSequence, 1);
        QVERIFY(!older.hasMoreBefore);
        QCOMPARE(older.events.size(), 6);
        QCOMPARE(older.events.first().toObject().value("turn_sequence").toInteger(), 1);
        QCOMPARE(older.events.last().toObject().value("turn_sequence").toInteger(), 2);
    }

    void abandonedApprovalsExpireWithoutAnsweringAProvider() {
        ApprovalBroker broker;
        QSignalSpy resolutions(&broker, &ApprovalBroker::providerResolutionReady);
        QSignalSpy expired(&broker, &ApprovalBroker::expired);
        const QString approval =
            broker.request("session-1", 1, QJsonValue(0), "approval", {{"reason", "run"}});
        QVERIFY(!approval.isEmpty());
        // A restarted provider reuses request id 0; the old approval must never answer it.
        broker.abandonPending();
        QCOMPARE(resolutions.count(), 0);
        QCOMPARE(expired.count(), 1);
        QCOMPARE(expired.constFirst().constFirst().toString(), approval);
        QVERIFY(!broker.resolve(approval, "session-1", 1, "approval", {{"decision", "accept"}})
                     .accepted);
        QCOMPARE(resolutions.count(), 0);
    }

    void staleSessionUpsertDoesNotMoveCursorsBackwards() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        const StoredSession stale{"session-1", directory.path(), "codex", {}, {}, "ready", 1, 0, 0,
                                  {}};
        QVERIFY(journal.upsertSession(stale));
        QVERIFY(journal.appendEvent("session-1", 1, "user.message", {{"text", "one"}}) > 0);
        const qint64 second =
            journal.appendEvent("session-1", 1, "user.message", {{"text", "two"}});
        QVERIFY(second > 0);
        QVERIFY(journal.upsertSession(stale));
        QCOMPARE(journal.sessions().constFirst().lastCursor, second);
        QString error;
        QCOMPARE(journal.appendEvent("session-1", 1, "user.message", {{"text", "three"}}, &error),
                 second + 1);
        // A new generation still starts its own counters.
        StoredSession next = stale;
        next.generation = 2;
        QVERIFY(journal.upsertSession(next));
        QCOMPARE(journal.sessions().constFirst().lastCursor, 0);
    }

    void backfillNeverRenumbersPrunedSessions() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("journal.sqlite3");
        qint64 turnsBefore = 0;
        {
            LocalJournal journal(path);
            QVERIFY(journal.upsertSession(
                {"session-1", directory.path(), "codex", {}, {}, "ready", 1, 0, 0, {}}));
            qint64 cursor = 0;
            for (const QString& text : {"one", "two", "three"})
                cursor = journal.appendEvent("session-1", 1, "user.message", {{"text", text}});
            QVERIFY(cursor > 0);
            turnsBefore = journal.sessions().constFirst().currentTurnSequence;
            QVERIFY(turnsBefore >= 3);
            // Retention prunes the first turns; the title stays empty (attachment-only prompt).
            QVERIFY(journal.acknowledgeCursor("desktop", "session-1", 1, cursor - 1));
            StoredSession untitled = journal.sessions().constFirst();
            untitled.title.clear();
            QVERIFY(journal.upsertSession(untitled));
        }
        LocalJournal reopened(path);
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        QCOMPARE(reopened.sessions().constFirst().currentTurnSequence, turnsBefore);
        const QJsonArray remaining = reopened.eventsAfter("session-1", 1, 0);
        QVERIFY(!remaining.isEmpty());
        QCOMPARE(remaining.last().toObject().value("turn_sequence").toInteger(), turnsBefore);
    }

    void backfillsCompactedReplayForAnExistingJournal() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("journal.sqlite3");
        const QString connection =
            QStringLiteral("legacy-journal-%1").arg(QUuid::createUuid().toString());
        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(path);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(
                "CREATE TABLE sessions (id TEXT PRIMARY KEY, worktree_path TEXT NOT NULL, "
                "adapter TEXT NOT NULL, thread_id TEXT NOT NULL, turn_id TEXT NOT NULL, "
                "state TEXT NOT NULL, generation INTEGER NOT NULL, last_cursor INTEGER NOT NULL)"));
            QVERIFY(query.exec(
                "CREATE TABLE events (session_id TEXT NOT NULL, generation INTEGER NOT NULL, "
                "cursor INTEGER NOT NULL, event_type TEXT NOT NULL, payload TEXT NOT NULL, "
                "sent_at TEXT NOT NULL, PRIMARY KEY(session_id, generation, cursor))"));
            query.prepare("INSERT INTO sessions VALUES(?, ?, 'codex', '', '', 'ready', 1, 2)");
            query.addBindValue("session-1");
            query.addBindValue(directory.path());
            QVERIFY(query.exec());
            QVERIFY(query.exec("INSERT INTO events VALUES('session-1', 1, 1, 'assistant.delta', "
                               "'{\"item_id\":\"answer\",\"delta\":\"partial\"}', 'now')"));
            QVERIFY(query.exec("INSERT INTO events VALUES('session-1', 1, 2, 'item.completed', "
                               "'{\"item\":{\"id\":\"answer\",\"type\":\"agentMessage\","
                               "\"text\":\"final\"}}', 'now')"));
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);

        LocalJournal migrated(path);
        QVERIFY2(migrated.isOpen(), qPrintable(migrated.errorString()));
        const EventReplayPage replay = migrated.compactedEventsAfter("session-1", 1, 0, 500);
        QCOMPARE(replay.events.size(), 1);
        QCOMPARE(replay.events.first().toObject().value("event_type").toString(), "item.completed");
        QCOMPARE(replay.nextCursor, 2);
    }

    void replayPagesStayWithinTheIpcByteBudget() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY2(journal.upsertSession(
                     {"session-1", directory.path(), "codex", {}, {}, "ready", 1, 0, 0, {}}),
                 qPrintable(journal.errorString()));

        const QString padding(220 * 1024, QLatin1Char('x'));
        for (int index = 0; index < 4; ++index) {
            QCOMPARE(journal.appendEvent(
                         "session-1", 1, "item.completed",
                         {{"item_id", QStringLiteral("item-%1").arg(index)}, {"padding", padding}}),
                     index + 1);
        }

        const QJsonArray raw = journal.eventsAfter("session-1", 1, 0, 500);
        QVERIFY(raw.size() > 0);
        QVERIFY(raw.size() < 4);
        QVERIFY(QJsonDocument(QJsonObject{{"events", raw}}).toJson(QJsonDocument::Compact).size() <
                protocol::maximumFrameBytes);
        const EventReplayPage first = journal.compactedEventsAfter("session-1", 1, 0, 500);
        QCOMPARE(first.events.size(), raw.size());
        QVERIFY(QJsonDocument(QJsonObject{{"events", first.events}})
                    .toJson(QJsonDocument::Compact)
                    .size() < protocol::maximumFrameBytes);
        QVERIFY(first.nextCursor > 0);
        QVERIFY(first.nextCursor < 4);
        const EventReplayPage second =
            journal.compactedEventsAfter("session-1", 1, first.nextCursor, 500);
        QCOMPARE(first.events.size() + second.events.size(), 4);
        QCOMPARE(second.nextCursor, 4);
    }

    void approvalHasOneAuthoritativeResolutionAndExpires() {
        ApprovalBroker broker;
        QSignalSpy resolutions(&broker, &ApprovalBroker::providerResolutionReady);
        QSignalSpy expired(&broker, &ApprovalBroker::expired);

        const QString approval = broker.request("session-1", 4, 77, "approval", {});
        const auto stale =
            broker.resolve(approval, "session-1", 3, "approval", {{"decision", "accept"}});
        QCOMPARE(stale.errorCode, "stale_generation");
        QVERIFY(broker.resolve(approval, "session-1", 4, "approval", {{"decision", "accept"}})
                    .accepted);
        QCOMPARE(resolutions.size(), 1);
        QCOMPARE(resolutions.first().at(1).toJsonObject().value("decision").toString(), "accept");
        QCOMPARE(broker.resolve(approval, "session-1", 4, "approval", {{"decision", "decline"}})
                     .errorCode,
                 "already_resolved");

        const QString expiring = broker.request("session-1", 4, 78, "approval", {}, 1);
        QTRY_COMPARE_WITH_TIMEOUT(expired.size(), 1, 1000);
        QCOMPARE(expired.first().at(0).toString(), expiring);
        QCOMPARE(resolutions.size(), 2);
        QCOMPARE(resolutions.last().at(1).toJsonObject().value("decision").toString(), "decline");
    }

    void expiredMcpToolApprovalRepliesWithElicitationDecline() {
        ApprovalBroker broker;
        QSignalSpy resolutions(&broker, &ApprovalBroker::providerResolutionReady);
        QSignalSpy expired(&broker, &ApprovalBroker::expired);
        const QJsonObject details{{"request_type", "mcp_tool_call"},
                                  {"server_name", "docs"},
                                  {"reason", "Allow docs to run tool?"}};
        const QString expiring = broker.request("session-1", 4, 79, "approval", details, 1);
        QVERIFY(!expiring.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(expired.size(), 1, 1000);
        QCOMPARE(resolutions.size(), 1);
        QCOMPARE(resolutions.first().at(0).toJsonValue(), QJsonValue(79));
        QCOMPARE(resolutions.first().at(1).toJsonObject(), (QJsonObject{{"action", "decline"}}));
    }

    void permissionsWaitForUserAndCancelOnlyTheirSession() {
        ApprovalBroker broker;
        QSignalSpy decisions(&broker, &ApprovalBroker::providerResolutionReady);
        QSignalSpy expired(&broker, &ApprovalBroker::expired);
        const auto first = broker.request("one", 1, 10, "approval", {});
        const auto second = broker.request("two", 1, 20, "approval", {});
        const auto page = broker.pendingPage("one", 1).value("pending_approvals").toArray();
        QCOMPARE(page.size(), 1);
        QVERIFY(page.first().toObject().value("expires_at").toString().isEmpty());
        QTest::qWait(300); // Run the expiry sweep: indefinite requests must survive it.
        QCOMPARE(decisions.size(), 0);
        broker.cancelPending("one", 2); // stale generation cannot cancel a current request
        QCOMPARE(decisions.size(), 0);
        broker.cancelPending("one", 1);
        QCOMPARE(decisions.size(), 1);
        QCOMPARE(decisions.first().at(1).toJsonObject().value("decision"), "decline");
        QCOMPARE(expired.size(), 1);
        QCOMPARE(broker.pendingPage("two", 1).value("pending_approvals").toArray().size(), 1);
        QVERIFY(!broker.resolve(first, "one", 1, "approval", {{"decision", "accept"}}).accepted);
        QVERIFY(broker.resolve(second, "two", 1, "approval", {{"decision", "accept"}}).accepted);
    }

    void structuredQuestionsAreNormalizedRevisionedAndArbitrated() {
        ApprovalBroker broker;
        QSignalSpy requested(&broker, &ApprovalBroker::requested);
        QSignalSpy resolutions(&broker, &ApprovalBroker::providerResolutionReady);
        QSignalSpy rejected(&broker, &ApprovalBroker::providerRequestRejected);
        const QJsonArray providerQuestions{
            QJsonObject{{"id", "target"},
                        {"header", "Target"},
                        {"question", "Choose targets"},
                        {"multiSelect", true},
                        {"isOther", true},
                        {"options", QJsonArray{QJsonObject{{"label", "macOS"}},
                                               QJsonObject{{"label", "iOS"}}}}}};
        const QString request =
            broker.request("session-1", 4, 91, "input",
                           {{"item_id", "item-1"}, {"questions", providerQuestions}}, 60'000);
        QVERIFY(!request.isEmpty());
        QCOMPARE(requested.size(), 1);
        const QJsonObject details = requested.first().at(4).toJsonObject();
        QCOMPARE(details.value("request_id").toString(), request);
        QCOMPARE(details.value("revision").toInt(), 1);
        const QJsonObject question = details.value("questions").toArray().first().toObject();
        QCOMPARE(question.value("selection").toString(), "multiple");
        QCOMPARE(question.value("revision").toInt(), 1);
        const QJsonArray options = question.value("options").toArray();
        QCOMPARE(options.size(), 2);

        QCOMPARE(
            broker.resolve(request, "session-1", 4, "approval", {{"decision", "accept"}}).errorCode,
            "request_kind_conflict");

        const QJsonObject defer{{"request_id", request},
                                {"revision", 1},
                                {"action", "defer"},
                                {"answers", QJsonArray{}}};
        const ApprovalResolution deferred = broker.resolve(request, "session-1", 4, "input", defer);
        QVERIFY(deferred.accepted);
        QCOMPARE(deferred.resultCode, "deferred");
        QCOMPARE(resolutions.size(), 0);
        const ApprovalResolution repeatedDefer =
            broker.resolve(request, "session-1", 4, "input", defer);
        QVERIFY(repeatedDefer.accepted);
        QCOMPARE(repeatedDefer.resultCode, "already_deferred");
        QCOMPARE(resolutions.size(), 0);

        QJsonObject stale = defer;
        stale.insert("revision", 2);
        QCOMPARE(broker.resolve(request, "session-1", 4, "input", stale).errorCode,
                 "revision_conflict");
        QCOMPARE(broker.resolve(request, "session-2", 4, "input", defer).errorCode,
                 "stale_generation");

        const QJsonObject answer{
            {"request_id", request},
            {"revision", 1},
            {"action", "answer"},
            {"answers", QJsonArray{QJsonObject{
                            {"question_id", "target"},
                            {"question_revision", 1},
                            {"option_ids", QJsonArray{options.at(0).toObject().value("id"),
                                                      options.at(1).toObject().value("id")}},
                            {"custom_answer", "visionOS"}}}}};
        const ApprovalResolution accepted =
            broker.resolve(request, "session-1", 4, "input", answer);
        QVERIFY(accepted.accepted);
        QCOMPARE(accepted.resultCode, "accepted");
        QCOMPARE(resolutions.size(), 1);
        const QJsonArray providerAnswers = resolutions.first()
                                               .at(1)
                                               .toJsonObject()
                                               .value("answers")
                                               .toObject()
                                               .value("target")
                                               .toObject()
                                               .value("answers")
                                               .toArray();
        QCOMPARE(providerAnswers, QJsonArray({"macOS", "iOS", "visionOS"}));
        QCOMPARE(broker.resolve(request, "session-1", 4, "input", answer).errorCode,
                 "already_answered");

        QJsonArray tooMany;
        for (int index = 0; index < 5; ++index) {
            QJsonObject question = providerQuestions.first().toObject();
            question.insert("id", QStringLiteral("question-%1").arg(index));
            tooMany.append(question);
        }
        // Claude's AskUserQuestion sends up to four.
        QJsonArray four = tooMany;
        four.removeLast();
        QVERIFY(!broker.request("session-1", 4, 91, "input", {{"questions", four}}).isEmpty());
        QVERIFY(rejected.isEmpty());
        QCOMPARE(broker.request("session-1", 4, 92, "input", {{"questions", tooMany}}), "");
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(rejected.first().at(1).toString(), "invalid_question_request");
        QCOMPARE(resolutions.size(), 1);

        QJsonObject duplicateOptions = providerQuestions.first().toObject();
        duplicateOptions.insert("options", QJsonArray{QJsonObject{{"id", "same"}, {"label", "A"}},
                                                      QJsonObject{{"id", "same"}, {"label", "B"}}});
        QCOMPARE(broker.request("session-1", 4, 93, "input",
                                {{"questions", QJsonArray{duplicateOptions}}}),
                 "");
        QCOMPARE(rejected.size(), 2);

        QJsonObject repeatedLabels = providerQuestions.first().toObject();
        repeatedLabels.insert(
            "options", QJsonArray{QJsonObject{{"label", "Same"}}, QJsonObject{{"label", "Same"}}});
        const QString repeated = broker.request("session-1", 4, 94, "input",
                                                {{"questions", QJsonArray{repeatedLabels}}});
        QVERIFY(!repeated.isEmpty());
        const QJsonArray stableOptions = requested.last()
                                             .at(4)
                                             .toJsonObject()
                                             .value("questions")
                                             .toArray()
                                             .first()
                                             .toObject()
                                             .value("options")
                                             .toArray();
        QCOMPARE(stableOptions.size(), 2);
        QVERIFY(stableOptions.at(0).toObject().value("id") !=
                stableOptions.at(1).toObject().value("id"));
    }

    void marksSessionSeenCursorDurablyAcrossReopen() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("journal.sqlite3"));
        {
            LocalJournal journal(path);
            QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
            StoredSession stored;
            stored.id = QStringLiteral("seen-session");
            stored.worktreePath = QFileInfo(directory.path()).canonicalFilePath();
            stored.adapter = QStringLiteral("codex");
            stored.state = QStringLiteral("ready");
            stored.lastCursor = 7;
            QVERIFY2(journal.upsertSession(stored), qPrintable(journal.errorString()));
            CodexAdapter adapter(QStringLiteral("/nonexistent/ketplus-test-codex"));
            SessionHost host(&journal, &adapter);
            const QJsonObject listed = host.handleCommand(
                QJsonObject{{QStringLiteral("protocol_version"), 0},
                            {QStringLiteral("message_id"), QUuid::createUuid().toString()},
                            {QStringLiteral("message_type"), QStringLiteral("session.list")},
                            {QStringLiteral("payload"), QJsonObject{}}},
                QStringLiteral("test"));
            QVERIFY(listed.value(QStringLiteral("ok")).toBool());
            const QJsonArray sessions = listed.value(QStringLiteral("sessions")).toArray();
            QVERIFY(!sessions.isEmpty());
            const QJsonObject session = sessions.at(0).toObject();
            QCOMPARE(session.value(QStringLiteral("last_event_cursor")).toInteger(), 7);
            QCOMPARE(session.value(QStringLiteral("last_seen_cursor")).toInteger(), 0);

            const QJsonObject marked = host.handleCommand(
                QJsonObject{
                    {QStringLiteral("protocol_version"), 0},
                    {QStringLiteral("message_id"), QUuid::createUuid().toString()},
                    {QStringLiteral("message_type"), QStringLiteral("session.mark_seen")},
                    {QStringLiteral("session_id"), QStringLiteral("seen-session")},
                    {QStringLiteral("session_generation"), 1},
                    {QStringLiteral("payload"), QJsonObject{{QStringLiteral("cursor"), 7}}}},
                QStringLiteral("test"));
            QVERIFY2(marked.value(QStringLiteral("ok")).toBool(),
                     qPrintable(QJsonDocument(marked).toJson()));
            QCOMPARE(marked.value(QStringLiteral("last_seen_cursor")).toInteger(), 7);
            QCOMPARE(journal.sessions().constFirst().lastSeenCursor, 7);
            // Stale or lower cursors must not rewind the marker.
            QVERIFY(journal.markSessionSeen(QStringLiteral("seen-session"), 1, 3));
            QCOMPARE(journal.sessions().constFirst().lastSeenCursor, 7);
        }
        LocalJournal reopened(path);
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        QCOMPARE(reopened.sessions().constFirst().lastSeenCursor, 7);
    }

    // Desktop and phones order conversations by this one journal time, not by when each of them
    // happened to notice a turn.
    void stampsTheLastTurnStartFromTheJournal() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("journal.sqlite3"));
        const auto sessionNamed = [](LocalJournal& journal, const QString& id) {
            for (const StoredSession& session : journal.sessions())
                if (session.id == id)
                    return session;
            return StoredSession{};
        };
        QString messageSentAt;
        {
            LocalJournal journal(path);
            QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
            StoredSession stored;
            stored.id = QStringLiteral("ordered-session");
            stored.worktreePath = QFileInfo(directory.path()).canonicalFilePath();
            stored.adapter = QStringLiteral("codex");
            stored.state = QStringLiteral("ready");
            QVERIFY2(journal.upsertSession(stored), qPrintable(journal.errorString()));
            QVERIFY(journal.appendEvent(stored.id, 1, QStringLiteral("session.created"), {}) > 0);
            QVERIFY(sessionNamed(journal, stored.id).lastTurnStartedAt.isEmpty());

            const qint64 cursor =
                journal.appendEvent(stored.id, 1, QStringLiteral("user.message"),
                                    {{QStringLiteral("text"), QStringLiteral("hello")}});
            QVERIFY(cursor > 0);
            messageSentAt = journal.eventsAfter(stored.id, 1, cursor - 1, 1)
                                .first()
                                .toObject()
                                .value(QStringLiteral("sent_at"))
                                .toString();
            QVERIFY(!messageSentAt.isEmpty());
            QVERIFY(journal.appendEvent(stored.id, 1, QStringLiteral("turn.completed"), {}) > 0);
            QCOMPARE(sessionNamed(journal, stored.id).lastTurnStartedAt, messageSentAt);
            // An upsert from a stale in-memory copy leaves the journal's time alone.
            QVERIFY(journal.upsertSession(stored));
            QCOMPARE(sessionNamed(journal, stored.id).lastTurnStartedAt, messageSentAt);

            CodexAdapter adapter(QStringLiteral("/nonexistent/ketplus-test-codex"));
            SessionHost host(&journal, &adapter);
            const QJsonObject listed = host.handleCommand(
                QJsonObject{{QStringLiteral("protocol_version"), 0},
                            {QStringLiteral("message_id"), QUuid::createUuid().toString()},
                            {QStringLiteral("message_type"), QStringLiteral("session.list")},
                            {QStringLiteral("payload"), QJsonObject{}}},
                QStringLiteral("test"));
            QCOMPARE(listed.value(QStringLiteral("sessions"))
                         .toArray()
                         .at(0)
                         .toObject()
                         .value(QStringLiteral("last_turn_started_at"))
                         .toString(),
                     messageSentAt);

            // A fork is listed first, as a conversation that just started a turn.
            StoredSession forked;
            forked.id = QStringLiteral("forked-session");
            forked.worktreePath = stored.worktreePath;
            forked.adapter = stored.adapter;
            forked.state = QStringLiteral("ready");
            QString forkError;
            QVERIFY2(journal.forkSession(stored.id, 1, forked, &forkError), qPrintable(forkError));
            QVERIFY(forked.lastTurnStartedAt >= messageSentAt);
            QCOMPARE(sessionNamed(journal, forked.id).lastTurnStartedAt, forked.lastTurnStartedAt);
        }
        // A journal from before the column existed recovers the time from its events.
        const QString connection = QStringLiteral("last-turn-started-test");
        {
            QSqlDatabase database =
                QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(path);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral("UPDATE sessions SET last_turn_started_at = ''")));
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        LocalJournal reopened(path);
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        QCOMPARE(sessionNamed(reopened, QStringLiteral("ordered-session")).lastTurnStartedAt,
                 messageSentAt);
        // The fork's copied history dates it at the source's last message.
        QCOMPARE(sessionNamed(reopened, QStringLiteral("forked-session")).lastTurnStartedAt,
                 messageSentAt);
    }

    void serviceCreatesProtectedStateAndRejectsEndpointCollision() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const DaemonConfiguration configuration{
            directory.filePath("state/journal.sqlite3"), directory.filePath("state/local.token"),
            directory.filePath("daemon.sock"), QStringLiteral("/nonexistent/ketplus-test-codex")};
        DaemonService service(configuration);
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        QVERIFY(QFile::exists(configuration.statePath));
        QVERIFY(QFile::exists(configuration.tokenPath));
        QFile token(configuration.tokenPath);
        QVERIFY(token.open(QIODevice::ReadOnly));
        QVERIFY(token.readAll().trimmed().size() >= 32);
#if defined(Q_OS_UNIX)
        constexpr QFileDevice::Permissions unsafePermissions =
            QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
            QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        QCOMPARE(QFileInfo(configuration.statePath).permissions() & unsafePermissions,
                 QFileDevice::Permissions{});
        QCOMPARE(QFileInfo(configuration.tokenPath).permissions() & unsafePermissions,
                 QFileDevice::Permissions{});
#endif
        DaemonService conflicting(configuration);
        QVERIFY(!conflicting.start());
        QVERIFY(!conflicting.errorString().isEmpty());
    }
};

QTEST_GUILESS_MAIN(DaemonProtocolTest)
#include "DaemonProtocolTest.moc"
