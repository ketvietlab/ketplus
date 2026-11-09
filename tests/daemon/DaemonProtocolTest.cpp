#include "protocol/DaemonProtocol.h"
#include "characterization/ProjectionChecks.h"
#include "cloud/CloudControlBatching.h"
#include "cloud/CloudEventProjection.h"
#include "cloud/WorktreeBranches.h"
#include "cloud/WorktreeRepositories.h"
#include "providers/CodexAdapter.h"
#include "runtime/ApprovalBroker.h"
#include "runtime/SessionHost.h"
#include "service/DaemonService.h"
#include "storage/LocalJournal.h"
#include "storage/McpMediaStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtTest>
#include <limits>
#ifdef Q_OS_MACOS
#include <cerrno>
#include <sys/acl.h>
#endif
#ifdef Q_OS_UNIX
#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#endif

using namespace ketplus::daemon;

#ifdef Q_OS_MACOS
// True when the path carries extended entries; an unreadable ACL counts as having them.
static bool hasExtendedAclEntries(const QString& path) {
    errno = 0;
    acl_t acl = acl_get_link_np(QFile::encodeName(path).constData(), ACL_TYPE_EXTENDED);
    if (acl == nullptr)
        return errno != ENOENT;
    acl_entry_t entry = nullptr;
    const bool any = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry) == 0;
    acl_free(acl);
    return any;
}
#endif

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
            return QJsonObject{{QStringLiteral("id"), id},
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
        const QJsonArray merged{
            codexModel, QJsonObject{{QStringLiteral("id"), QStringLiteral("claude-model")},
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
        QCOMPARE(chosen.value(QStringLiteral("execution_policy")).toString(),
                 QStringLiteral("full"));
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
        QVERIFY(store.copySessionReferences("session", "fork"));
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

    void localImageFifoIsRejectedWithoutBlocking() {
#ifdef Q_OS_UNIX
        // A viewed path that is (or became, after any earlier check) a FIFO nobody writes.
        // The store opens before it checks the type, so the open itself must not block.
        QTemporaryDir directory;
        McpMediaStore store(directory.filePath("media"));
        const QString path = directory.filePath("shot.png");
        QCOMPARE(::mkfifo(QFile::encodeName(path).constData(), 0600), 0);
        std::atomic<bool> finished{false};
        QString reason;
        std::thread reader([&] {
            reason = store.externalizeItem({{"type", "imageView"},
                                            { "path",
                                              path }},
                                           "session")
                         .value("result")
                         .toObject()
                         .value("content")
                         .toArray()
                         .first()
                         .toObject()
                         .value("reason")
                         .toString();
            finished = true;
        });
        QElapsedTimer waited;
        waited.start();
        while (!finished && waited.elapsed() < 2000)
            QThread::msleep(10);
        const bool blocked = !finished;
        if (blocked) {
            // Release the blocked open before reporting it.
            const int writer = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_NONBLOCK);
            if (writer >= 0)
                ::close(writer);
        }
        reader.join();
        QVERIFY2(!blocked, "Reading a viewed image blocked on a FIFO");
        QCOMPARE(reason, QString("source_unavailable"));
#else
        QSKIP("FIFOs are POSIX-only");
#endif
    }

    void startupRetainDeletesMediaNoReferenceNames() {
        QTemporaryDir directory;
        McpMediaStore store(directory.filePath("media"));
        const auto png = [](char fill) {
            return QByteArray("\x89PNG\r\n\x1a\n") + QByteArray(32, fill);
        };
        const auto file = [&](const QJsonObject& reference) {
            return QDir(store.directory())
                .filePath(reference.value("media_ref").toString().mid(7) + ".png");
        };
        const auto onlyA = store.storeImage(png('a'), "image/png", "a");
        const auto shared = store.storeImage(png('s'), "image/png", "a");
        QVERIFY(!store.storeImage(png('s'), "image/png", "b").isEmpty());
        const auto onlyB = store.storeImage(png('b'), "image/png", "b");
        // Stopped after removing a deleted session's reference file, before its media.
        const auto released = store.storeImage(png('r'), "image/png", "gone");
        QVERIFY(QFile::remove(
            QDir(store.directory())
                .filePath("refs/" +
                          QCryptographicHash::hash("gone", QCryptographicHash::Sha256).toHex() +
                          ".refs")));
        // Stopped after writing media, before referencing it.
        const QByteArray unreferenced = png('u');
        const QString written =
            QDir(store.directory())
                .filePath(
                    QCryptographicHash::hash(unreferenced, QCryptographicHash::Sha256).toHex() +
                    ".png");
        QFile write(written);
        QVERIFY(write.open(QIODevice::WriteOnly));
        write.write(unreferenced);
        write.close();
        // Not a name the store writes: left alone.
        QFile foreign(QDir(store.directory()).filePath("notes.png"));
        QVERIFY(foreign.open(QIODevice::WriteOnly));
        foreign.close();
        for (const auto& path : {file(onlyA), file(shared), file(onlyB), file(released), written})
            QVERIFY(QFileInfo::exists(path));

        store.retainSessions({"b"});
        QVERIFY(!QFileInfo::exists(file(onlyA)));
        QVERIFY(QFileInfo::exists(file(shared))); // still referenced by b
        QVERIFY(QFileInfo::exists(file(onlyB)));
        QVERIFY(!QFileInfo::exists(file(released)));
        QVERIFY(!QFileInfo::exists(written));
        QVERIFY(QFileInfo::exists(foreign.fileName()));
#ifdef Q_OS_UNIX
        // A special file with a canonical name is not media and stays.
        const QString special =
            QDir(store.directory()).filePath(QString(64, QChar('f')) + QStringLiteral(".png"));
        QCOMPARE(::mkfifo(QFile::encodeName(special).constData(), 0600), 0);
        store.retainSessions({"b"});
        QVERIFY(QFileInfo::exists(special));
        QVERIFY(QFileInfo::exists(file(onlyB)));
#endif
    }

    void startupRetainDoesNotFollowALinkedMediaRoot() {
        // The store's directory is a link to another directory that looks like a store.
        QTemporaryDir directory;
        const QString elsewhere = directory.filePath("elsewhere");
        QVERIFY(QDir().mkpath(elsewhere + "/refs"));
        const QString victim = elsewhere + '/' + QString(64, QChar('e')) + ".png";
        QFile file(victim);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("\x89PNG\r\n\x1a\n");
        file.close();
        const QString media = directory.filePath("media");
        QVERIFY(QFile::link(elsewhere, media));
        McpMediaStore store(media);
        store.retainSessions({});
        store.releaseSession("any");
        QVERIFY(QFileInfo::exists(victim));
    }

    void mediaOwnershipCommitFailurePreservesPreviousFile() {
#ifdef Q_OS_UNIX
        QProcess probe;
        probe.setProcessChannelMode(QProcess::MergedChannels);
        probe.start(QCoreApplication::applicationFilePath(), {"--media-reference-write-fault"});
        QVERIFY(probe.waitForStarted());
        QVERIFY(probe.waitForFinished(30000));
        const auto output = probe.readAll();
        QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0,
                 qPrintable(QStringLiteral("exit %1: %2")
                                .arg(probe.exitCode())
                                .arg(QString::fromUtf8(output))));
#else
        QSKIP("The isolated file-size write-failure fixture requires POSIX resource limits.");
#endif
    }

    void mediaPublicationRequiresStoredOwnership_data() {
        QTest::addColumn<QString>("fault");
        QTest::addColumn<QString>("entry");
        for (const QString& fault :
             {QStringLiteral("valid"), QStringLiteral("directory"), QStringLiteral("symlink"),
              QStringLiteral("dangling"), QStringLiteral("malformed"), QStringLiteral("oversized"),
              QStringLiteral("unreadable"), QStringLiteral("root-symlink"),
              QStringLiteral("root-file")})
            for (const QString& entry :
                 {QStringLiteral("attachment"), QStringLiteral("inline"), QStringLiteral("local")})
                QTest::newRow(qPrintable(fault + '-' + entry)) << fault << entry;
    }

    void mediaPublicationRequiresStoredOwnership() {
        QFETCH(QString, fault);
        QFETCH(QString, entry);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto media = directory.filePath("media");
        const auto refs = media + "/refs";
        const auto backup = directory.filePath("reference-backup");
        const auto referencePath =
            refs + '/' + QCryptographicHash::hash("a", QCryptographicHash::Sha256).toHex() +
            ".refs";
        const auto put = [](const QString& path, const QByteArray& bytes) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        const auto oldImage = fakePng(8, 6, 'o');
        const auto newImage = fakePng(8, 6, 'n');
        const auto oldHash = QCryptographicHash::hash(oldImage, QCryptographicHash::Sha256).toHex();
        const auto newHash = QCryptographicHash::hash(newImage, QCryptographicHash::Sha256).toHex();
        McpMediaStore store(media);
        QVERIFY(!store.storeImage(oldImage, "image/png", "a").isEmpty());
        QVERIFY(!store.storeImage(oldImage, "image/png", "b").isEmpty());
        const auto original = oldHash + '\n';
        const auto foreign = directory.filePath("foreign.refs");
        QVERIFY(put(foreign, original));
        QByteArray damaged;
        if (fault == "root-symlink" || fault == "root-file") {
            QVERIFY(QDir().rename(refs, backup));
            if (fault == "root-symlink")
                QVERIFY(QFile::link(backup, refs));
            else
                QVERIFY(put(refs, "unsupported root"));
        } else if (fault == "directory" || fault == "symlink" || fault == "dangling") {
            QVERIFY(QFile::remove(referencePath));
            if (fault == "directory")
                QVERIFY(QDir().mkpath(referencePath));
            else
                QVERIFY(QFile::link(fault == "symlink" ? foreign : directory.filePath("missing"),
                                    referencePath));
        } else if (fault == "malformed" || fault == "oversized") {
            damaged = fault == "malformed"
                          ? original + "not-a-digest\n"
                          : original.repeated(4 * 1024 * 1024 / original.size() + 1);
            QVERIFY(put(referencePath, damaged));
        } else if (fault == "unreadable") {
            QVERIFY(QFile::setPermissions(referencePath, QFileDevice::WriteOwner));
        }
        const auto imagePath = directory.filePath("source.png");
        QVERIFY(put(imagePath, newImage));
        const auto publish = [&]() {
            if (entry == "attachment")
                return QJsonArray{store.storeImage(newImage, "image/png", "a")};
            QJsonObject item;
            if (entry == "local") {
                item = {{"type", "imageView"}, {"path", imagePath}};
            } else {
                const auto data = QString::fromLatin1(newImage.toBase64());
                item = {{"type", "mcpToolCall"},
                        {"result",
                         QJsonObject{
                             {"content",
                              QJsonArray{
                                  QJsonObject{{"type", "text"}, {"text", "keep text"}},
                                  QJsonObject{
                                      {"type", "image"}, {"mimeType", "image/png"}, {"data", data}},
                                  QJsonObject{{"type", "resource"},
                                              {"resource", QJsonObject{{"uri", "synthetic://image"},
                                                                       {"mimeType", "image/png"},
                                                                       {"blob", data}}}}}}}}};
            }
            return store.externalizeItem(item, "a")
                .value("result")
                .toObject()
                .value("content")
                .toArray();
        };
        const auto first = publish();
        const int begin = entry == "inline" ? 1 : 0;
        if (entry == "inline")
            QCOMPARE(first.first().toObject().value("text").toString(), "keep text");
        for (int i = begin; i < first.size(); ++i) {
            const auto block = first[i].toObject();
            QCOMPARE(block.contains("media_ref"), fault == "valid");
            if (fault != "valid" && entry != "attachment")
                QVERIFY(block.value("omitted").toBool());
        }
        if (fault != "valid") {
            if (fault == "root-symlink" || fault == "root-file") {
                QVERIFY(QFile::remove(refs));
                QVERIFY(QDir().rename(backup, refs));
            } else if (fault == "directory") {
                QVERIFY(QDir().rmdir(referencePath));
            } else if (fault == "symlink" || fault == "dangling") {
                QVERIFY(QFile::remove(referencePath));
            } else if (fault == "unreadable") {
                QVERIFY(QFile::setPermissions(referencePath,
                                              QFileDevice::ReadOwner | QFileDevice::WriteOwner));
            } else {
                QFile unchanged(referencePath);
                QVERIFY(unchanged.open(QIODevice::ReadOnly));
                QCOMPARE(unchanged.readAll(), damaged);
            }
            QVERIFY(put(referencePath, original));
        }
        for (int retry = 0; retry < 2; ++retry) {
            const auto result = publish();
            for (int i = begin; i < result.size(); ++i)
                QCOMPARE(result[i].toObject().value("media_ref").toString(),
                         "sha256:" + QString::fromLatin1(newHash));
        }
        QFile saved(referencePath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        auto hashes = saved.readAll().split('\n');
        hashes.removeAll(QByteArray{});
        QCOMPARE(hashes.size(), 2); // repeated/inline-duplicate publication does not grow metadata
        QVERIFY(hashes.contains(oldHash));
        QVERIFY(hashes.contains(newHash));
        QVERIFY(!(saved.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                         QFileDevice::ReadOther | QFileDevice::WriteOther)));
        saved.close();
        QFile outside(foreign);
        QVERIFY(outside.open(QIODevice::ReadOnly));
        QCOMPARE(outside.readAll(), original);
        store.releaseSession("b");
        QVERIFY(QFileInfo::exists(media + '/' + oldHash + ".png"));
        QVERIFY(QFileInfo::exists(media + '/' + newHash + ".png"));
        store.releaseSession("a");
        QVERIFY(QDir(media).entryList({"*.png"}, QDir::Files).isEmpty());
    }

    void mediaDedupRequiresVerifiedStoredBytes_data() {
        QTest::addColumn<QString>("fault");
        QTest::addColumn<bool>("inlineItem");
        for (const QString& fault :
             {QStringLiteral("valid"), QStringLiteral("wide-permissions"),
              QStringLiteral("corrupt"), QStringLiteral("short"), QStringLiteral("oversized"),
              QStringLiteral("directory"), QStringLiteral("symlink"), QStringLiteral("dangling"),
              QStringLiteral("unreadable"), QStringLiteral("root-symlink")}) {
            for (const bool inlineItem : {false, true})
                QTest::newRow(qPrintable(fault + (inlineItem ? "-item" : "-attachment")))
                    << fault << inlineItem;
        }
    }

    void mediaDedupRequiresVerifiedStoredBytes() {
        QFETCH(QString, fault);
        QFETCH(bool, inlineItem);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto png = fakePng(8, 6, 'd');
        const auto hash = QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex();
        const auto media = directory.filePath("media");
        const auto outside = directory.filePath("outside");
        QVERIFY(QDir().mkpath(outside));
        const auto path = media + '/' + QString::fromLatin1(hash) + ".png";
        const auto foreign = outside + '/' + QString::fromLatin1(hash) + ".png";
        const auto originalBytes = fault == "corrupt" ? QByteArray(png.size(), 'x')
                                   : fault == "short" ? png.left(10)
                                   : fault == "oversized"
                                       ? QByteArray(McpMediaStore::maximumImageBytes + 1, 'x')
                                       : png;
        const auto put = [](const QString& target, const QByteArray& bytes) {
            QFile file(target);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        QVERIFY(put(foreign, png));
        if (fault == "root-symlink")
            QVERIFY(QFile::link(outside, media));
        else
            QVERIFY(QDir().mkpath(media));
        if (fault == "directory") {
            QVERIFY(QDir().mkpath(path));
        } else if (fault == "symlink" || fault == "dangling") {
            QVERIFY(QFile::link(fault == "symlink" ? foreign : outside + "/missing", path));
        } else if (fault != "root-symlink") {
            QVERIFY(put(path, originalBytes));
            if (fault == "valid")
                QVERIFY(
                    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
            if (fault == "unreadable")
                QVERIFY(QFile::setPermissions(path, QFileDevice::WriteOwner));
            if (fault == "wide-permissions")
                QVERIFY(QFile::setPermissions(path,
                                              QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ReadGroup | QFileDevice::ReadOther));
        }
        const auto publish = [&]() {
            McpMediaStore store(media);
            if (!inlineItem)
                return store.storeImage(png, "image/png", "session");
            const QJsonObject item{
                {"type", "mcpToolCall"},
                {"result",
                 QJsonObject{{"content", QJsonArray{QJsonObject{
                                             {"type", "image"},
                                             {"mimeType", "image/png"},
                                             {"data", QString::fromLatin1(png.toBase64())}}}}}}};
            return store.externalizeItem(item, "session")
                .value("result")
                .toObject()
                .value("content")
                .toArray()
                .first()
                .toObject();
        };
        const auto result = publish();
        const bool valid = fault == "valid" || fault == "wide-permissions";
        QCOMPARE(result.contains("media_ref"), valid);
        if (!valid) {
            if (inlineItem)
                QCOMPARE(result.value("reason").toString(), "storage_unavailable");
            QVERIFY(!QFileInfo::exists(media + "/refs"));
            if (fault == "corrupt" || fault == "short" || fault == "oversized") {
                QFile original(path);
                QVERIFY(original.open(QIODevice::ReadOnly));
                QCOMPARE(original.readAll(), originalBytes);
            }
            if (fault == "root-symlink") {
                QVERIFY(QFile::remove(media));
                QVERIFY(QDir().mkpath(media));
            } else if (fault == "directory") {
                QVERIFY(QDir().rmdir(path));
            } else {
                if (fault == "unreadable")
                    QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner |
                                                            QFileDevice::WriteOwner));
                QVERIFY(QFile::remove(path));
            }
            QCOMPARE(publish().value("media_ref").toString(),
                     "sha256:" + QString::fromLatin1(hash));
        }
        QFile retained(path);
        QVERIFY(retained.open(QIODevice::ReadOnly));
        QCOMPARE(retained.readAll(), png);
        QVERIFY(!(retained.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                            QFileDevice::ReadOther | QFileDevice::WriteOther)));
        QFile preserved(foreign);
        QVERIFY(preserved.open(QIODevice::ReadOnly));
        QCOMPARE(preserved.readAll(), png);
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

    void mediaCapacityReclaimsFilesWithAnEmptyCatalog() {
        for (bool present : {false, true}) {
            QTemporaryDir dir;
            const auto media = dir.filePath("media");
            QVERIFY(QDir().mkpath(media));
            const auto png = fakePng(8, 8, 'a');
            const auto path = QDir(media).filePath(
                QString::fromLatin1(
                    QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex()) +
                ".png");
            QFile image(path);
            QVERIFY(image.open(QIODevice::WriteOnly));
            QCOMPARE(image.write(png), png.size());
            image.close();
            if (present)
                QVERIFY(QDir().mkpath(QDir(media).filePath("refs")));
            McpMediaStore store(media, 0);
            store.enforceCapacity();
            QVERIFY(!QFileInfo::exists(path));
        }
    }

    void mediaCleanupPreservesUnknownReferences_data() {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<QString>("fault");
        QTest::addColumn<QString>("owner");
        for (const auto& operation : {QString("release"), QString("retain"), QString("capacity")})
            for (const auto& fault :
                 {QString("malformed"), QString("oversized"), QString("directory"),
                  QString("symlink"), QString("catalog-count"), QString("catalog-bytes"),
                  QString("root-symlink"), QString("filename")})
                for (const auto& owner : {QString("a"), QString("b")})
                    if ((!fault.startsWith("catalog-") && fault != "root-symlink" &&
                         fault != "filename") ||
                        owner == "a")
                        QTest::newRow(qPrintable(operation + "-" + fault + "-" + owner))
                            << operation << fault << owner;
    }
    void mediaCleanupPreservesUnknownReferences() {
        QFETCH(QString, operation);
        QFETCH(QString, fault);
        QFETCH(QString, owner);
        QTemporaryDir dir;
        const auto media = dir.filePath("media");
        McpMediaStore store(media);
        const auto png = fakePng(8, 8, 'a');
        const auto first = store.storeImage(png, "image/png", "a");
        QCOMPARE(store.storeImage(png, "image/png", "b"), first);
        QVERIFY(!first.isEmpty());
        const auto hash = first.value("media_ref").toString().mid(7);
        const auto image = QDir(media).filePath(hash + ".png");
        const auto refPath = [&](const QString& id) {
            return QDir(media).filePath(
                "refs/" +
                QString::fromLatin1(
                    QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex()) +
                ".refs");
        };
        const auto broken = refPath(owner);
        const auto original = hash.toLatin1() + '\n';
        QVERIFY(QFile::remove(broken));
        const auto write = [](const QString& path, const QByteArray& contents) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(contents) == contents.size();
        };
        QStringList extraFiles;
        const auto refsDirectory = QDir(media).filePath("refs");
        const auto outside = dir.filePath("outside-refs");
        if (fault == "filename") {
            QVERIFY(write(broken, original));
            const auto unknown = refPath("unknown").chopped(5) + ".future.refs";
            QVERIFY(write(unknown, original));
            extraFiles.append(unknown);
        } else if (fault == "root-symlink") {
            QVERIFY(write(broken, original));
            QVERIFY(QDir().rename(refsDirectory, outside));
            QVERIFY(QFile::link(outside, refsDirectory));
            if (!QFileInfo(refsDirectory).isSymLink())
                QSKIP("Fixture requires a native directory symlink.");
        } else if (fault.startsWith("catalog-")) {
            QVERIFY(write(broken, original));
            const int count = fault == "catalog-count" ? 4096 : 5;
            const auto bytes = fault == "catalog-count"
                                   ? QByteArray{}
                                   : original.repeated(4 * 1024 * 1024 / original.size());
            for (int n = 0; n < count; ++n) {
                const auto path = refPath(QString("extra-%1").arg(n));
                QVERIFY(write(path, bytes));
                extraFiles.append(path);
            }
        } else if (fault == "directory")
            QVERIFY(QDir().mkdir(broken));
        else if (fault == "symlink") {
            const auto target = dir.filePath("outside.refs");
            QVERIFY(write(target, original));
            QVERIFY(QFile::link(target, broken));
            if (!QFileInfo(broken).isSymLink())
                QSKIP("Fixture requires a native filesystem symlink.");
        } else {
            const auto damaged = fault == "oversized"
                                     ? original.repeated(4 * 1024 * 1024 / original.size() + 1)
                                     : QByteArray("unknown-reference-format\n");
            QVERIFY(write(broken, damaged));
        }
        if (operation == "release")
            store.releaseSession("a");
        else if (operation == "retain")
            store.retainSessions({"b"});
        else {
            McpMediaStore tiny(media, 0);
            tiny.enforceCapacity();
        }
        QVERIFY2(QFileInfo::exists(image),
                 "Unknown ownership must not authorize deleting shared bytes.");
        QVERIFY2(QFileInfo::exists(refPath("a")),
                 "Failed preflight must preserve retired references for retry.");
        QVERIFY(QFileInfo::exists(refPath("b")));
        if (fault == "root-symlink") {
            QVERIFY(QFile::remove(refsDirectory));
            QVERIFY(QDir().rename(outside, refsDirectory));
        } else if (fault == "directory")
            QVERIFY(QDir().rmdir(broken));
        else
            QVERIFY(QFile::remove(broken));
        QVERIFY(write(broken, original));
        for (const auto& path : extraFiles)
            QVERIFY(QFile::remove(path));
        store.releaseSession("a");
        QVERIFY(QFileInfo::exists(image));
        store.releaseSession("b");
        QVERIFY(!QFileInfo::exists(image));
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
                        {"item",
                         QJsonObject{{"id", "read-1"},
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
        QVERIFY(git(repository, {"symbolic-ref", "refs/remotes/origin/HEAD",
                                 "refs/remotes/origin/release/1.0"}));

        const RepositoryBranches branches = readRepositoryBranches(repository);
        QVERIFY(branches.ok);
        QCOMPARE(branches.local, (QStringList{"develop", "main"}));
        QCOMPARE(branches.remote, QStringList{"origin/release/1.0"});

        const AddedWorktree added = addWorktree(repository, QStringLiteral("feat/phone"),
                                                QStringLiteral("origin/release/1.0"));
        QVERIFY2(added.errorCode.isEmpty(), qPrintable(added.message));
        QCOMPARE(added.path, root.filePath(QStringLiteral("app-feat-phone")));
        QCOMPARE(checkoutBranch(added.path), QStringLiteral("feat/phone"));
        QCOMPARE(worktreeRepository(added.path), repository);
        QCOMPARE(repositoryCheckouts(repository), (QStringList{repository, added.path}));

        QCOMPARE(addWorktree(repository, "main", "develop").errorCode,
                 QStringLiteral("branch_exists"));
        QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("app-taken"))));
        QCOMPARE(addWorktree(repository, "taken", "develop").errorCode,
                 QStringLiteral("worktree_exists"));
        QCOMPARE(addWorktree(repository, "fresh", "HEAD~1").errorCode,
                 QStringLiteral("base_branch_not_found"));
        QCOMPARE(addWorktree(repository, "fresh", "--orphan").errorCode,
                 QStringLiteral("base_branch_not_found"));
        for (const QString& invalid :
             {"", "has space", "-x", "a..b", "a/", "a.lock", "x/.y", "a:b", "@"})
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
        for (const auto& mode : {QString("all"), QString("one")}) {
            for (bool large : {false, true})
                QTest::newRow(qPrintable(QString("replay-remove-%1-%2").arg(mode).arg(large)))
                    << "replay-remove-" + mode << large;
            QTest::newRow(qPrintable("command-remove-" + mode))
                << "command-remove-" + mode << false;
            for (const auto& target : {QString("item"), QString("summary")})
                for (const auto& event : {QString("complete"), QString("error")})
                    QTest::newRow(qPrintable("settle-" + target + "-" + mode + "-" + event))
                        << "settle-" + target + "-" + mode + "-" + event << false;
        }
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
        const bool remove =
            fault.startsWith("replay-remove-") || fault.startsWith("command-remove-");
        const bool settle = fault.startsWith("settle-");
        if (remove)
            for (int n = 0; n < 2; ++n)
                QVERIFY(journal.appendEvent("s", 1,
                                            fault.startsWith("command-") ? "command.output_delta"
                                                                         : "assistant.delta",
                                            {{"item_id", "probe"}, {"delta", "old"}}) > 1);
        if (settle)
            for (int n = 1; n <= 3; ++n)
                QVERIFY(journal.appendEvent(
                            "s", 1, "item.started",
                            {{"item_id", QString("probe-%1").arg(n)},
                             {"item_type", "mcpToolCall"},
                             {"item",
                              QJsonObject{{"id", QString("probe-%1").arg(n)},
                                          {"type", "mcpToolCall"},
                                          {"tool", n == 3 ? "search_files" : "read_file"}}}}) > 1);
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
            if (remove) {
                table = "replay_events";
                action = "DELETE";
                if (fault.endsWith("one"))
                    condition = "WHEN OLD.cursor=3";
            }
            if (settle) {
                table = fault.startsWith("settle-item-") ? "activity_items" : "activity_summaries";
                action = "UPDATE";
                if (fault.contains("-one-"))
                    condition = table == "activity_items" ? "WHEN OLD.item_id='probe-2'"
                                                          : "WHEN OLD.family='search'";
            }
            QVERIFY(q.exec(QString("CREATE TRIGGER fail_append BEFORE %1 ON %2 %3 "
                                   "BEGIN SELECT RAISE(IGNORE); END")
                               .arg(action, table, condition)));
        }
        const bool item = large || remove || fault == "revision" || fault.startsWith("activity-");
        const QString type = settle ? (fault.endsWith("error") ? "runtime.error" : "turn.completed")
                             : item ? "item.completed"
                                    : "assistant.delta";
        const QString itemType = fault.startsWith("activity-") ? "mcpToolCall" : "agentMessage";
        QJsonObject payload =
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
        if (fault.startsWith("command-remove-"))
            payload = {{"item_id", "probe"},
                       {"item_type", "commandExecution"},
                       {"item", QJsonObject{{"id", "probe"},
                                            {"type", "commandExecution"},
                                            {"aggregatedOutput", "done"}}}};
        if (settle)
            payload = {{"status", "completed"}};
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

    void appendPreservesUnsupportedCounters_data() {
        QTest::addColumn<QString>("cursorSql");
        QTest::addColumn<QString>("turnSql");
        QTest::addColumn<bool>("large");
        QTest::addColumn<bool>("userMessage");
        QTest::addColumn<bool>("accepted");
        for (bool large : {false, true}) {
            for (const auto& value :
                 {QString("-1"), QString("1.5"), QString("'unknown'"), QString("X'01'")}) {
                QTest::newRow(qPrintable(QString("cursor-%1-%2").arg(large).arg(value)))
                    << value << QString("1") << large << false << false;
                QTest::newRow(qPrintable(QString("turn-%1-%2").arg(large).arg(value)))
                    << QString("1") << value << large << false << false;
            }
        }
        const auto maximum = QString::number(std::numeric_limits<qint64>::max());
        QTest::newRow("cursor-exhausted") << maximum << QString("1") << false << false << false;
        QTest::newRow("turn-exhausted") << QString("1") << maximum << false << true << false;
        QTest::newRow("large-cursor-overflow")
            << QString::number(std::numeric_limits<qint64>::max() - 2) << QString("1") << true
            << false << false;
        QTest::newRow("cursor-last-slot") << QString::number(std::numeric_limits<qint64>::max() - 1)
                                          << QString("1") << false << false << true;
        QTest::newRow("turn-last-slot")
            << QString("1") << QString::number(std::numeric_limits<qint64>::max() - 1) << false
            << true << true;
        QTest::newRow("maximum-turn-no-increment")
            << QString("1") << maximum << false << false << true;
        QTest::newRow("large-last-slots")
            << QString("large-fit") << QString("1") << true << false << true;
    }
    void appendPreservesUnsupportedCounters() {
        QFETCH(QString, cursorSql);
        QFETCH(QString, turnSql);
        QFETCH(bool, large);
        QFETCH(bool, userMessage);
        QFETCH(bool, accepted);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(journal.upsertSession({"s", dir.path(), "codex", {}, {}, "ready", 1, 0, 0, {}}));
        QCOMPARE(journal.appendEvent("s", 1, "user.message", {{"text", "seed"}}), 1);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        const QJsonObject payload =
            large
                ? QJsonObject{{"item_id", "probe"},
                              {"item_type", "agentMessage"},
                              {"item",
                               QJsonObject{
                                   {"id", "probe"},
                                   {"type", "agentMessage"},
                                   {"text", QString(2 * protocol::maximumEventPayloadBytes, 'x')}}}}
                : QJsonObject{{"text", "new"}, {"delta", "new"}};
        const qint64 chunks =
            (QJsonDocument(payload).toJson(QJsonDocument::Compact).size() + 96 * 1024 - 1) /
            (96 * 1024);
        if (cursorSql == "large-fit")
            cursorSql = QString::number(std::numeric_limits<qint64>::max() - chunks - 1);
        QSqlQuery q(db);
        QVERIFY(q.exec(
            QString("UPDATE sessions SET last_cursor=%1,current_turn_sequence=%2 WHERE id='s'")
                .arg(cursorSql, turnSql)));
        const auto snapshot = [&] {
            QStringList values;
            QSqlQuery query(db);
            for (const auto& table : {"events", "replay_events", "large_event_payloads",
                                      "large_event_payload_chunks", "activity_items"}) {
                if (!query.exec(QString("SELECT count(*) FROM %1").arg(table)) || !query.next())
                    return QStringList{};
                values.append(query.value(0).toString());
            }
            if (!query.exec("SELECT quote(last_cursor),quote(current_turn_sequence) FROM sessions "
                            "WHERE id='s'") ||
                !query.next())
                return QStringList{};
            values.append(query.value(0).toString());
            values.append(query.value(1).toString());
            return values;
        };
        const auto before = snapshot();
        QCOMPARE(before.size(), 7);
        QString error;
        const auto result = journal.appendEvent("s", 1,
                                                large         ? "item.completed"
                                                : userMessage ? "user.message"
                                                              : "assistant.delta",
                                                payload, &error);
        if (!accepted) {
            QCOMPARE(result, qint64(-1));
            QVERIFY(!error.isEmpty());
            QCOMPARE(snapshot(), before);
        } else {
            QCOMPARE(result, cursorSql.toLongLong() + (large ? chunks + 1 : 1));
            QVERIFY(q.exec("SELECT current_turn_sequence FROM sessions WHERE id='s'"));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toLongLong(), turnSql.toLongLong() + (userMessage ? 1 : 0));
        }
        QVERIFY2(q.exec("BEGIN"), qPrintable(q.lastError().text()));
        QVERIFY(q.exec("ROLLBACK"));
        // Repair this temporary fixture; retry must not inherit an open transaction.
        QVERIFY(
            q.exec("UPDATE sessions SET last_cursor=1000,current_turn_sequence=1 WHERE id='s'"));
        QCOMPARE(journal.appendEvent("s", 1, "assistant.delta", {{"delta", "retry"}}, &error),
                 qint64(1001));
        LocalJournal reopened(path);
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        QCOMPARE(reopened.sessionHead("s").cursor, qint64(1001));
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

    void startupCommitFailurePreservesErrorAndReleasesTransaction_data() {
        QTest::addColumn<QString>("phase");
        QTest::addColumn<QString>("operation");
        // Activity indexing no longer runs at open (ADR 0097); JournalStartupTest covers it.
        for (const auto& phase : {"turn", "replay"})
            for (const auto& operation : {"none", "read-policy", "write-policy", "write-session"}) {
                const auto name = QString("%1-%2").arg(phase, operation).toLatin1();
                QTest::newRow(name.constData()) << QString(phase) << QString(operation);
            }
    }
    void startupCommitFailurePreservesErrorAndReleasesTransaction() {
        QFETCH(QString, phase);
        QFETCH(QString, operation);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        const auto snapshot = [](QSqlDatabase db) {
            QVariantMap result;
            for (const auto& table :
                 {"sessions", "events", "replay_events", "replay_projection_state",
                  "activity_heads", "activity_items", "activity_summaries", "startup_child",
                  "journal_retention_settings", "journal_size_settings"}) {
                QSqlQuery q(db);
                if (!q.exec(QString("SELECT * FROM %1 ORDER BY 1,2,3").arg(table))) {
                    QTest::qFail(qPrintable(q.lastError().text()), __FILE__, __LINE__);
                    return QVariantMap{};
                }
                QVariantList values;
                while (q.next())
                    for (int n = 0; n < q.record().count(); ++n)
                        values.append(q.value(n));
                result.insert(table, values);
            }
            return result;
        };
        QVariantMap before;
        {
            LocalJournal seed(path);
            QVERIFY(seed.isOpen());
            QVERIFY(seed.upsertSession({"s", dir.path(), "codex", {}, {}, "ready"}));
            QCOMPARE(seed.appendEvent("s", 1, "user.message", {{"text", "seed"}}), qint64(1));
            QCOMPARE(seed.appendEvent("s", 1, "item.started",
                                      {{"item", QJsonObject{{"id", "probe"},
                                                            {"type", "mcpToolCall"},
                                                            {"tool", "read_file"}}}}),
                     qint64(2));
            QSqlDatabase db;
            for (const auto& name : QSqlDatabase::connectionNames())
                if (name.startsWith("ketplus-daemon-") &&
                    QSqlDatabase::database(name).databaseName() == path)
                    db = QSqlDatabase::database(name);
            QVERIFY(db.isOpen());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE startup_parent(id INTEGER PRIMARY KEY)"));
            QVERIFY(q.exec("CREATE TABLE startup_child(id INTEGER REFERENCES startup_parent(id) "
                           "DEFERRABLE INITIALLY DEFERRED, marker INTEGER, value INTEGER)"));
            QVERIFY(q.exec("UPDATE sessions SET title='fixture'"));
            QString mutation;
            if (phase == "turn") {
                // Turn numbers are backfilled only in a journal from before the format marker.
                QVERIFY(q.exec("PRAGMA user_version=0"));
                QVERIFY(q.exec("UPDATE sessions SET current_turn_sequence=0"));
                QVERIFY(q.exec("UPDATE events SET turn_sequence=0"));
                mutation = "AFTER UPDATE ON sessions WHEN "
                           "NEW.current_turn_sequence>OLD.current_turn_sequence";
            } else if (phase == "replay") {
                QVERIFY(q.exec("DELETE FROM replay_events"));
                QVERIFY(q.exec("DELETE FROM replay_projection_state"));
                mutation = "AFTER INSERT ON replay_projection_state";
            } else {
                QVERIFY(q.exec("DELETE FROM activity_heads"));
                QVERIFY(q.exec("DELETE FROM activity_items"));
                QVERIFY(q.exec("DELETE FROM activity_summaries"));
                mutation = "AFTER INSERT ON activity_heads";
            }
            QVERIFY(q.exec(QString("CREATE TRIGGER fail_startup %1 BEGIN INSERT INTO startup_child "
                                   "VALUES(1,0,0); END")
                               .arg(mutation)));
            before = snapshot(db);
        }
        LocalJournal failed(path);
        QVERIFY2(!failed.isOpen(), "Failed startup must never advertise an initialized journal.");
        QVERIFY2(failed.errorString().contains("FOREIGN KEY", Qt::CaseInsensitive),
                 qPrintable(failed.errorString()));
        // No public operation may revive or write through an incompletely initialized object.
        if (operation == "read-policy")
            QVERIFY(!failed.retentionPolicy());
        else if (operation == "write-policy")
            QVERIFY(!failed.setRetentionPolicy({7, true, 0}, 0));
        else if (operation == "write-session")
            QVERIFY(!failed.upsertSession({"unexpected", dir.path(), "codex", {}, {}, "ready"}));
        QVERIFY(!failed.isOpen());
        const auto connection = QString("startup-repair-%1").arg(phase);
        const auto remove = qScopeGuard([&] { QSqlDatabase::removeDatabase(connection); });
        auto repair = QSqlDatabase::addDatabase("QSQLITE", connection);
        repair.setConnectOptions("QSQLITE_BUSY_TIMEOUT=30");
        repair.setDatabaseName(path);
        QVERIFY(repair.open());
        QSqlQuery q(repair);
        // Keep the failed object alive: startup must release its write transaction itself.
        QVERIFY2(q.exec("BEGIN IMMEDIATE"), qPrintable(q.lastError().text()));
        QVERIFY(q.exec("ROLLBACK"));
        QCOMPARE(snapshot(repair), before);
        QVERIFY(q.exec("DROP TRIGGER fail_startup"));
        LocalJournal reopened(path);
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        QCOMPARE(reopened.sessionHead("s").cursor, qint64(2));
        QCOMPARE(reopened.eventsAfter("s", 1, 0).size(), 2);
        QCOMPARE(reopened.compactedEventsAfter("s", 1, 0, 10).events.size(), 2);
    }

    void failedAgentSchemaCannotBeUsedUntilReopened() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("journal.sqlite");
        {
            LocalJournal seed(path);
            QVERIFY(seed.isOpen());
            QVERIFY(seed.upsertSession({"s", dir.path(), "codex", {}, {}, "ready"}));
            QCOMPARE(seed.appendEvent("s", 1, "user.message", {{"text", "seed"}}), qint64(1));
        }
        const auto connection = QUuid::createUuid().toString();
        const auto remove = qScopeGuard([&] { QSqlDatabase::removeDatabase(connection); });
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec("INSERT INTO conversation_numbers VALUES ('1234','s',1,'synthetic','')"));
        QVERIFY(q.exec("DROP INDEX conversation_numbers_session"));
        QVERIFY(q.exec("ALTER TABLE conversation_numbers RENAME COLUMN generation "
                       "TO unsupported_generation"));
        LocalJournal failed(path);
        QVERIFY(!failed.isOpen());
        QVERIFY2(failed.errorString().contains("generation"), qPrintable(failed.errorString()));
        QVERIFY(!failed.retentionPolicy());
        QVERIFY(!failed.setRetentionPolicy({7, true, 0}, 0));
        QVERIFY(!failed.upsertSession({"unexpected", dir.path(), "codex", {}, {}, "ready"}));
        QVERIFY(!failed.isOpen());
        QVERIFY(q.exec("SELECT COUNT(*) FROM sessions"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);
        q.finish();
        QVERIFY(q.exec("SELECT COUNT(*) FROM journal_retention_settings"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
        q.finish();
        QVERIFY(q.exec("ALTER TABLE conversation_numbers RENAME COLUMN unsupported_generation "
                       "TO generation"));
        QVERIFY(!failed.isOpen());
        LocalJournal reopened(path);
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        QCOMPARE(reopened.eventsAfter("s", 1, 0).size(), 1);
        QCOMPARE(reopened.conversationNumber("s", 1), QString("1234"));
        QVERIFY(reopened.setRetentionPolicy({7, true, 0}, 0));
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
            QVERIFY(query.exec(QStringLiteral("PRAGMA user_version=0")));
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

    void serviceRefusesAStateDirectoryItCannotRestrict() {
#ifdef Q_OS_UNIX
        // The shared temporary directory is writable but owned by root, so chmod on it fails,
        // as it would for any state directory owned by someone else.
        const QString shared =
            QFileInfo(QStringLiteral("/private/tmp")).isDir() ? "/private/tmp" : "/tmp";
        const QFileInfo sharedInfo(shared);
        if (!sharedInfo.isWritable() || sharedInfo.ownerId() == ::getuid())
            QSKIP("Needs a writable directory owned by another user");
        QTemporaryDir directory;
        const QString name = "ketplus-state-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const DaemonConfiguration configuration{
            shared + '/' + name + ".sqlite3", shared + '/' + name + ".token",
            directory.filePath("daemon.sock"), QStringLiteral("/nonexistent/ketplus-test-codex")};
        const auto cleanup = qScopeGuard([&] {
            for (const auto& suffix :
                 {".sqlite3", ".sqlite3-wal", ".sqlite3-shm", ".sqlite3.lock", ".token"})
                QFile::remove(shared + '/' + name + suffix);
        });
        DaemonService service(configuration);
        QVERIFY(!service.start());
        QVERIFY2(service.errorString().contains("restrict"), qPrintable(service.errorString()));
        QVERIFY(!QFileInfo::exists(configuration.tokenPath));
#else
        QSKIP("POSIX directory ownership");
#endif
    }

    void runningServiceKeepsItsLocksWhenTheyAge() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const DaemonConfiguration configuration{
            directory.filePath("state/journal.sqlite3"), directory.filePath("state/local.token"),
            directory.filePath("daemon.sock"), QStringLiteral("/nonexistent/ketplus-test-codex")};
        DaemonService service(configuration);
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        // A daemon that has run for an hour: its lock files were written at startup.
        int aged = 0;
        QDirIterator locks(directory.path(), {"*.lock"}, QDir::Files | QDir::Hidden,
                           QDirIterator::Subdirectories);
        while (locks.hasNext()) {
            QFile lock(locks.next());
            QVERIFY(lock.open(QIODevice::ReadWrite));
            QVERIFY(lock.setFileTime(QDateTime::currentDateTime().addSecs(-3600),
                                     QFileDevice::FileModificationTime));
            ++aged;
        }
        QCOMPARE(aged, 2);
        DaemonService second(configuration);
        QVERIFY(!second.start());
        QVERIFY2(second.errorString().contains("Another KetPlus Daemon"),
                 qPrintable(second.errorString()));
        QLocalSocket client;
        client.connectToServer(configuration.socketName);
        QVERIFY(client.waitForConnected(2'000));
    }

    void serviceRemovesInheritedAccessEntriesFromItsState() {
#ifdef Q_OS_MACOS
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString state = directory.filePath("state");
        QVERIFY(QDir().mkpath(state));
        // Everything created below inherits a grant the owner-only mode bits do not show.
        QCOMPARE(
            QProcess::execute("/bin/chmod", {"+a",
                                             "everyone allow list,search,read,readattr,readextattr,"
                                             "readsecurity,file_inherit,directory_inherit",
                                             state}),
            0);
        const DaemonConfiguration configuration{state + "/journal.sqlite3", state + "/local.token",
                                                directory.filePath("daemon.sock"),
                                                QStringLiteral("/nonexistent/ketplus-test-codex")};
        const QByteArray exposedToken(43, 'k');
        QFile token(configuration.tokenPath);
        QVERIFY(token.open(QIODevice::WriteOnly));
        QVERIFY(token.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QCOMPARE(token.write(exposedToken), exposedToken.size());
        token.close();
        const QString media = state + "/media";
        QVERIFY(QDir().mkpath(media + "/refs"));
        // A journal from before the fix: SQLite opens an empty file as an empty database.
        const QStringList written{configuration.statePath, media + "/foreign.bin",
                                  media + "/refs/session.refs"};
        for (const auto& path : written) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
            if (path != configuration.statePath)
                file.write("x");
        }
        for (const auto& path :
             QStringList{configuration.tokenPath, media, media + "/refs"} + written)
            QVERIFY2(hasExtendedAclEntries(path), qPrintable(path));

        auto service = std::make_unique<DaemonService>(configuration);
        QVERIFY2(service->start(), qPrintable(service->errorString()));
        QStringList checked{state, media, media + "/refs", configuration.tokenPath,
                            configuration.statePath};
        checked += written;
        for (const auto& suffix : {"-wal", "-shm"})
            if (QFileInfo::exists(configuration.statePath + suffix))
                checked.append(configuration.statePath + suffix);
        for (const auto& path : checked) {
            QVERIFY2(QFileInfo::exists(path), qPrintable(path));
            QVERIFY2(!hasExtendedAclEntries(path), qPrintable(path));
        }
        // Files the daemon creates later inherit nothing.
        QFile later(state + "/later");
        QVERIFY(later.open(QIODevice::WriteOnly));
        later.close();
        QVERIFY(!hasExtendedAclEntries(later.fileName()));
        // A credential others could read was replaced, not trusted.
        QVERIFY(token.open(QIODevice::ReadOnly));
        const QByteArray current = token.readAll().trimmed();
        QVERIFY(current.size() >= 32);
        QVERIFY(current != exposedToken);
        token.close();

        // A journal already in its final format is opened in place, not rewritten, so an
        // entry added to it later must be removed too.
        service.reset();
        QCOMPARE(
            QProcess::execute("/bin/chmod", {"+a", "everyone allow read", configuration.statePath}),
            0);
        QVERIFY(hasExtendedAclEntries(configuration.statePath));
        DaemonService restarted(configuration);
        QVERIFY2(restarted.start(), qPrintable(restarted.errorString()));
        QVERIFY(!hasExtendedAclEntries(configuration.statePath));
#else
        QSKIP("macOS access-control entries");
#endif
    }

    void rejectedSecondDaemonKeepsTheRunningCredential() {
#ifdef Q_OS_MACOS
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const DaemonConfiguration configuration{
            directory.filePath("state/journal.sqlite3"), directory.filePath("state/local.token"),
            directory.filePath("daemon.sock"), QStringLiteral("/nonexistent/ketplus-test-codex")};
        DaemonService running(configuration);
        QVERIFY2(running.start(), qPrintable(running.errorString()));
        const auto readToken = [&configuration] {
            QFile file(configuration.tokenPath);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
        };
        const QByteArray served = readToken();
        QVERIFY(!served.isEmpty());
        // Even a credential that now looks exposed belongs to the daemon that owns the journal.
        QCOMPARE(
            QProcess::execute("/bin/chmod", {"+a", "everyone allow read", configuration.tokenPath}),
            0);
        DaemonService second(configuration);
        QVERIFY(!second.start());
        QVERIFY2(second.errorString().contains("Another KetPlus Daemon"),
                 qPrintable(second.errorString()));
        QCOMPARE(readToken(), served);
#else
        QSKIP("macOS access-control entries");
#endif
    }

    void linkedStateDirectoryLosesItsInheritableEntries() {
#ifdef Q_OS_MACOS
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString target = directory.filePath("real");
        const QString alias = directory.filePath("alias");
        QVERIFY(QDir().mkpath(target));
        QVERIFY(QFile::link(target, alias));
        QCOMPARE(
            QProcess::execute("/bin/chmod", {"+a",
                                             "everyone allow list,search,read,readattr,readextattr,"
                                             "readsecurity,file_inherit,directory_inherit",
                                             target}),
            0);
        QVERIFY(hasExtendedAclEntries(target));
        const DaemonConfiguration configuration{alias + "/journal.sqlite3", alias + "/local.token",
                                                directory.filePath("daemon.sock"),
                                                QStringLiteral("/nonexistent/ketplus-test-codex")};
        DaemonService service(configuration);
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        QVERIFY(!hasExtendedAclEntries(target));
        QVERIFY(!hasExtendedAclEntries(configuration.tokenPath));
        QVERIFY(!hasExtendedAclEntries(configuration.statePath));
#else
        QSKIP("macOS access-control entries");
#endif
    }

    void unreadableAccessEntriesCountAsExposed() {
#ifdef Q_OS_MACOS
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString state = directory.filePath("state");
        QVERIFY(QDir().mkpath(state));
        const DaemonConfiguration configuration{state + "/journal.sqlite3", state + "/local.token",
                                                directory.filePath("daemon.sock"),
                                                QStringLiteral("/nonexistent/ketplus-test-codex")};
        const QByteArray exposed(43, 'k');
        QFile token(configuration.tokenPath);
        QVERIFY(token.open(QIODevice::WriteOnly));
        QVERIFY(token.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QCOMPARE(token.write(exposed), exposed.size());
        token.close();
        // The grant to others is there, but reading the ACL fails even for the owner.
        QCOMPARE(
            QProcess::execute("/bin/chmod", {"+a", "everyone allow read", configuration.tokenPath}),
            0);
        QCOMPARE(QProcess::execute("/bin/chmod",
                                   {"+a", "everyone deny readsecurity", configuration.tokenPath}),
                 0);
        const auto cleanup =
            qScopeGuard([&] { QProcess::execute("/bin/chmod", {"-N", configuration.tokenPath}); });
        errno = 0;
        QCOMPARE(acl_get_link_np(QFile::encodeName(configuration.tokenPath).constData(),
                                 ACL_TYPE_EXTENDED),
                 acl_t{});
        QCOMPARE(errno, EACCES);
        DaemonService service(configuration);
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        QVERIFY(token.open(QIODevice::ReadOnly));
        QVERIFY(token.readAll().trimmed() != exposed);
        QVERIFY(!hasExtendedAclEntries(configuration.tokenPath));
#else
        QSKIP("macOS access-control entries");
#endif
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
        // Another user's process cannot even open the endpoint (with the daemon's own
        // UserAccessOption and umask 077, either alone gives this).
        QVERIFY(QFileInfo::exists(configuration.socketName));
        QCOMPARE(QFileInfo(configuration.socketName).permissions() & unsafePermissions,
                 QFileDevice::Permissions{});
#endif
        DaemonService conflicting(configuration);
        QVERIFY(!conflicting.start());
        QVERIFY(!conflicting.errorString().isEmpty());
    }
};

#ifdef Q_OS_UNIX
static int mediaReferenceWriteFailureProbe() {
    QTemporaryDir directory;
    if (!directory.isValid())
        return 10;
    const auto media = directory.filePath("media");
    McpMediaStore store(media);
    const auto oldImage = QByteArray::fromHex("89504e470d0a1a0a0000000d494844520000000800000006");
    auto newImage = oldImage;
    newImage[newImage.size() - 1] = '\x07';
    if (store.storeImage(oldImage, "image/png", "a").isEmpty())
        return 11;
    const auto refs = media + "/refs/" +
                      QCryptographicHash::hash("a", QCryptographicHash::Sha256).toHex() + ".refs";
    const auto read = [&]() {
        QFile file(refs);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    const auto original = read();
    if (original.size() != 65)
        return 12;
    struct rlimit previous{};
    if (getrlimit(RLIMIT_FSIZE, &previous) != 0)
        return 13;
    auto limited = previous;
    limited.rlim_cur = 90; // both 24-byte images fit; the two-hash reference file cannot
    std::signal(SIGXFSZ, SIG_IGN);
    if (setrlimit(RLIMIT_FSIZE, &limited) != 0)
        return 14;
    const auto restore = qScopeGuard([&] { setrlimit(RLIMIT_FSIZE, &previous); });
    const auto attachment = store.storeImage(newImage, "image/png", "a");
    const QJsonObject image{{"type", "image"},
                            {"mimeType", "image/png"},
                            {"data", QString::fromLatin1(newImage.toBase64())}};
    const auto item = store.externalizeItem(
        {{"type", "mcpToolCall"}, {"result", QJsonObject{{"content", QJsonArray{image}}}}}, "a");
    const auto block =
        item.value("result").toObject().value("content").toArray().first().toObject();
    if (setrlimit(RLIMIT_FSIZE, &previous) != 0)
        return 15;
    if (!attachment.isEmpty() || block.contains("media_ref") || !block.value("omitted").toBool()) {
        qWarning("Reference persistence failure was reported as successful media publication");
        return 16;
    }
    if (read() != original) {
        qWarning("Failed reference commit modified existing ownership");
        return 17;
    }
    if (store.storeImage(newImage, "image/png", "a").isEmpty() || read().size() != 130)
        return 18;
    return 0;
}
#endif

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    application.setAttribute(Qt::AA_Use96Dpi, true);
#ifdef Q_OS_UNIX
    if (application.arguments().contains("--media-reference-write-fault"))
        return mediaReferenceWriteFailureProbe();
#endif
    DaemonProtocolTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "DaemonProtocolTest.moc"
