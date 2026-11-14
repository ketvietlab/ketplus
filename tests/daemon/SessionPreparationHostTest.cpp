#include "FakeCodex.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace ketplus::daemon;
namespace {
struct Fixture {
    QTemporaryDir root;
    QString log = root.filePath("provider.log");
    LocalJournal journal{root.filePath("journal.sqlite")};
    CodexAdapter provider{root.filePath("fake-codex")};
    SessionHost host{&journal, &provider};
    Fixture() { writeFakeCodex(root.filePath("fake-codex"), log); }
    QJsonObject run(const QString& type, const QJsonObject& payload = {}, const QString& id = {},
                    const QString& device = {}) {
        QJsonObject request{{"message_id", QUuid::createUuid().toString()},
                            {"message_type", type},
                            {"payload", payload},
                            {"session_id", id},
                            {"session_generation", 1}};
        if (!device.isEmpty())
            request.insert("_verified_sender_device_id", device);
        return host.handleCommand(request, device.isEmpty() ? "test-local" : "cloud-control-v2");
    }
    QJsonObject prepare(const QString& request = "draft-1", const QString& device = {}) {
        return run("session.prepare",
                   {{"client_request_id", request}, {"worktree_path", root.path()}}, {}, device);
    }
    QJsonObject stage(const QString& id, const QString& device = {}) {
        const QByteArray bytes = "first-turn reference";
        const auto digest = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        auto result = run("attachment.stage.begin",
                          {{"attachment_id", "file-1"},
                           {"expected_revision", 0},
                           {"name", "reference.txt"},
                           {"media_type", "text/plain"},
                           {"total_bytes", bytes.size()},
                           {"chunk_count", 1},
                           {"digest_sha256", digest}},
                          id, device);
        if (!result.value("ok").toBool())
            return result;
        result = run("attachment.stage.chunk",
                     {{"attachment_id", "file-1"},
                      {"expected_revision", 1},
                      {"chunk_index", 0},
                      {"data", QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding))},
                      {"digest_sha256", digest}},
                     id, device);
        return result.value("ok").toBool()
                   ? run("attachment.stage.commit",
                         {{"attachment_id", "file-1"}, {"expected_revision", 1}}, id, device)
                   : result;
    }
};
} // namespace
class SessionPreparationHostTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
    QByteArray oldData_;
    QByteArray oldLogInput_;
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("dev.ketplus.tests.preparation");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        QVERIFY(QSettings().fileName().startsWith(settings_.path()));
        QVERIFY(!QSettings().contains("workspace/lastRoot"));
        oldData_ = qgetenv("KETPLUS_DAEMON_DATA_DIR");
        oldLogInput_ = qgetenv("KETPLUS_FAKE_CODEX_LOG_INPUT");
        qputenv("KETPLUS_FAKE_CODEX_LOG_INPUT", "1");
        qputenv("KETPLUS_DAEMON_DATA_DIR", settings_.filePath("data").toUtf8());
    }
    void cleanupTestCase() {
        if (oldLogInput_.isNull())
            qunsetenv("KETPLUS_FAKE_CODEX_LOG_INPUT");
        else
            qputenv("KETPLUS_FAKE_CODEX_LOG_INPUT", oldLogInput_);
        if (oldData_.isNull())
            qunsetenv("KETPLUS_DAEMON_DATA_DIR");
        else
            qputenv("KETPLUS_DAEMON_DATA_DIR", oldData_);
    }
    void prepareStageAndFirstTurn() {
        Fixture f;
        const auto result = f.prepare();
        QVERIFY(result.value("ok").toBool());
        const auto id = result.value("session_id").toString();
        QCOMPARE(f.prepare().value("session_id"), result.value("session_id"));
        QVERIFY(f.journal.sessions().isEmpty());
        QVERIFY(f.journal.eventsAfter(id, 1, 0).isEmpty());
        QVERIFY(!QFile::exists(f.log));
        QVERIFY(f.stage(id).value("ok").toBool());
        QVERIFY(f.journal.sessions().isEmpty());
        const auto started =
            f.run("session.start_turn",
                  {{"prompt", "read the reference"}, {"attachment_ids", QJsonArray{"file-1"}}}, id);
        QVERIFY2(started.value("ok").toBool(), QJsonDocument(started).toJson().constData());
        QCOMPARE(f.journal.sessions().size(), 1);
        QCOMPARE(f.journal.sessionPreparation(id)->state, "activated");
        QTRY_VERIFY([&] {
            QFile log(f.log);
            return log.open(QIODevice::ReadOnly) && log.readAll().contains("first-turn reference");
        }());
        QCOMPARE(f.run("session.prepare.cancel", {}, id).value("error_code").toString(),
                 "preparation_consumed");
        QCOMPARE(f.journal.sessions().size(), 1);
    }
    void cancellationIsIdempotentAndErasesTheStagedFile() {
        Fixture f;
        const auto id = f.prepare().value("session_id").toString();
        QVERIFY(f.stage(id).value("ok").toBool());
        QVERIFY(f.run("session.prepare.cancel", {}, id).value("ok").toBool());
        QVERIFY(f.run("session.prepare.cancel", {}, id).value("ok").toBool());
        QVERIFY(f.journal.sessions().isEmpty());
        QVERIFY(f.journal.attachment(id, 1, "file-1").status.isEmpty());
        QVERIFY(!f.prepare().value("ok").toBool());
        QVERIFY(!QFile::exists(f.log));
    }
    void preparedSessionIsPrivateAndCannotBeReadAsAConversation() {
        Fixture f;
        const auto id = f.prepare("draft-1", "device-a").value("session_id").toString();
        QVERIFY(!id.isEmpty());
        QCOMPARE(f.stage(id, "device-b").value("error_code").toString(), "preparation_not_found");
        QCOMPARE(f.run("session.prepare.cancel", {}, id, "device-b").value("error_code").toString(),
                 "preparation_not_found");
        QCOMPARE(
            f.run("session.configuration.read", {}, id, "device-a").value("error_code").toString(),
            "preparation_inactive");
        QVERIFY(f.stage(id, "device-a").value("ok").toBool());
        QVERIFY(f.journal.sessions().isEmpty());
        QVERIFY(!QFile::exists(f.log));
    }
    void invalidScopeFailsWithoutConsumingThePreparation() {
        Fixture f;
        const auto id = f.prepare().value("session_id").toString();
        QDir().mkpath(f.root.filePath("other"));
        const auto result =
            f.run("session.start_turn",
                  {{"prompt", "read"}, {"worktree_path", f.root.filePath("other")}}, id);
        QCOMPARE(result.value("error_code").toString(), "preparation_scope_changed");
        QCOMPARE(f.journal.sessionPreparation(id)->state, "prepared");
        QVERIFY(f.journal.sessions().isEmpty());
        QVERIFY(!QFile::exists(f.log));
    }
    void expiryRejectsCommandsBeforeTheStartupSweepRemovesBytes() {
        Fixture f;
        SessionPreparation preparation{"expired",
                                       "local",
                                       "expired-draft",
                                       f.root.path(),
                                       {{"provider", "codex"}},
                                       QDateTime::currentMSecsSinceEpoch() - 1};
        QVERIFY(f.journal.prepareSession(preparation));
        const QByteArray bytes = "x";
        const auto digest = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        QVERIFY(f.journal.beginAttachment("expired", 1, "file-1", 0, "reference.txt", "text/plain",
                                          1, 1, digest));
        QVERIFY(f.journal.appendAttachmentChunk("expired", 1, "file-1", 1, 0, bytes, digest));
        QCOMPARE(f.run("session.start_turn", {{"prompt", "read"}}, "expired")
                     .value("error_code")
                     .toString(),
                 "preparation_expired");
        QVERIFY(f.journal.sessions().isEmpty());
        QTRY_COMPARE(f.journal.sessionPreparation("expired")->state, "expired");
        QVERIFY(f.journal.attachment("expired", 1, "file-1").status.isEmpty());
        QVERIFY(!QFile::exists(f.log));
    }
    void aRemovedWorktreeRejectsBytesButStillAllowsDraftCancellation() {
        Fixture f;
        const auto worktree = f.root.filePath("worktree");
        QVERIFY(QDir().mkpath(worktree));
        const auto result = f.run("session.prepare", {{"worktree_path", worktree},
                                                      {"client_request_id", "draft-removed"}});
        const auto id = result.value("session_id").toString();
        QVERIFY(!id.isEmpty());
        QVERIFY(QDir(worktree).removeRecursively());
        QCOMPARE(f.stage(id).value("error_code").toString(), "preparation_scope_changed");
        QVERIFY(f.run("session.prepare.cancel", {}, id).value("ok").toBool());
        QVERIFY(f.journal.sessions().isEmpty());
    }
    void ownershipIsCheckedBeforeTheOrdinaryCommandResultCache() {
        Fixture f;
        const auto id = f.prepare("draft-1", "device-a").value("session_id").toString();
        QJsonObject request{{"message_id", "same-command-id"},
                            {"message_type", "attachment.stage.begin"},
                            {"session_id", id},
                            {"session_generation", 1},
                            {"_verified_sender_device_id", "device-a"},
                            {"payload", QJsonObject{{"attachment_id", "file-1"},
                                                    {"expected_revision", 0},
                                                    {"name", "reference.txt"},
                                                    {"media_type", "text/plain"},
                                                    {"total_bytes", 1},
                                                    {"chunk_count", 1},
                                                    {"digest_sha256", QString(64, 'a')}}}};
        QVERIFY(f.host.handleCommand(request, "cloud-control-v2").value("ok").toBool());
        request.insert("_verified_sender_device_id", "device-b");
        QCOMPARE(f.host.handleCommand(request, "cloud-control-v2").value("error_code").toString(),
                 "preparation_not_found");
        QJsonObject deferred;
        f.host.handleCommandDeferred(request, "cloud-control-v2",
                                     [&](const QJsonObject& reply) { deferred = reply; });
        QCOMPARE(deferred.value("error_code").toString(), "preparation_not_found");
    }
};
QTEST_GUILESS_MAIN(SessionPreparationHostTest)
#include "SessionPreparationHostTest.moc"
