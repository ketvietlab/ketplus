#include "FakeCodex.h"
#include "MachineAttachmentUpload.h"
#include "SshFixture.h"
#include "ssh/SshMachineBackend.h"
#include "ssh/SshMachineStore.h"
#include <QSignalSpy>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>
#include <optional>

// ADR 0119 over SSH end to end: a real ketplus_daemon behind a loopback sshd, reached through
// SshTunnel and SshMachineBackend. The provider is FakeCodex, so no provider quota is used.
using namespace ketplus::remote::ssh;
using namespace ketplus::remote::ssh::testing;
using State = ketplus::remote::MachineBackend::ConnectionState;

namespace {

struct Reply {
    std::optional<QJsonObject> result;
    QString error;
    [[nodiscard]] bool done() const { return result.has_value(); }
    [[nodiscard]] auto callback() {
        return [this](const QJsonObject& value, const QString& message) {
            result = value;
            error = message;
        };
    }
};

QByteArray readToken(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray{};
}

} // namespace

class SshDaemonIntegrationTest : public QObject {
    Q_OBJECT

  private slots:
    void aRealDaemonServesTheMachineOverSsh() {
        SshFixture fixture;
        if (!fixture.start(true, false))
            QSKIP(qPrintable(fixture.skip));
        fixture.trust(fixture.hostKey);

        const QString data = fixture.path("data");
        const QString worktree = fixture.path("worktree");
        QVERIFY(QDir().mkpath(worktree));
        const QString codex = fixture.path("fake-codex");
        QVERIFY(writeFakeCodex(codex, fixture.path("provider.log")));
        // sshd names this file in KETPLUS_DAEMON_TOKEN_FILE; the real daemon creates it.
        const QString tokenFile = fixture.path("local.token");
        QVERIFY(QFile::remove(tokenFile));

        QProcess daemon;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("KETPLUS_DAEMON_DATA_DIR", data);
        daemon.setProcessEnvironment(environment);
        daemon.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        daemon.start(KETPLUS_TEST_DAEMON, {"--socket", fixture.path("daemon.sock"), "--state",
                                           QDir(data).filePath("daemon/journal.sqlite3"),
                                           "--token-file", tokenFile, "--codex", codex});
        QVERIFY(daemon.waitForStarted(5000));
        const auto stop = qScopeGuard([&daemon] {
            daemon.terminate();
            if (!daemon.waitForFinished(5000))
                daemon.kill();
        });
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(fixture.path("daemon.sock")) &&
                                     !readToken(tokenFile).isEmpty(),
                                 patience);

        SshMachineStore store(fixture.path("machines.json"));
        const SshTarget target{"fixture", {}, 0};
        QVERIFY(store.add(target));
        // The tunnel reads the token over the verified SSH route and keeps it in memory only.
        auto link = std::make_unique<SshTunnelLink>(target, fixture.options());
        SshTunnel* tunnel = link->tunnel();
        SshMachineBackend backend(target, &store, std::move(link));
        backend.connectToHost();
        QTRY_COMPARE_WITH_TIMEOUT(backend.connectionState(), State::Connected, patience);
        QCOMPARE(tunnel->state(), SshTunnel::State::Connected);
        QCOMPARE(backend.machineId().size(), 43);
        QCOMPARE(store.machineId("fixture"), backend.machineId());
        const QJsonObject details = backend.connectionDetails();
        QCOMPARE(details.value("route").toString(), QStringLiteral("SSH"));
        QCOMPARE(details.value("ssh_host_key_fingerprint").toString(),
                 fixture.hostKey.fingerprint());

        // A first turn registers the worktree and creates the conversation.
        Reply started;
        backend.startTurn({}, -1, {{"worktree_path", worktree}, {"prompt", "continue"}},
                          started.callback());
        QTRY_VERIFY_WITH_TIMEOUT(started.done(), patience);
        QVERIFY2(started.error.isEmpty(), qPrintable(started.error));
        const QString sessionId = started.result->value("session_id").toString();
        QVERIFY(!sessionId.isEmpty());
        const qint64 generation = started.result->value("session_generation").toInteger(1);

        Reply repositories;
        backend.listRepositories(repositories.callback());
        QTRY_VERIFY_WITH_TIMEOUT(repositories.done(), patience);
        QVERIFY2(repositories.error.isEmpty(), qPrintable(repositories.error));
        bool listed = false;
        for (const QJsonValue& value : repositories.result->value("repositories").toArray())
            listed = listed || value.toObject().value("path").toString() ==
                                   QFileInfo(worktree).canonicalFilePath();
        QVERIFY2(listed, QJsonDocument(*repositories.result).toJson().constData());

        const auto conversationListed = [&](SshMachineBackend& machine) {
            Reply conversations;
            machine.listConversations({}, conversations.callback());
            if (!QTest::qWaitFor([&] { return conversations.done(); }, patience))
                return false;
            for (const QJsonValue& value : conversations.result->value("sessions").toArray())
                if (value.toObject().value("session_id").toString() == sessionId)
                    return true;
            return false;
        };
        QVERIFY(conversationListed(backend));

        // The first turn completes; a second asks for an approval, answered through the backend.
        const auto turnCompleted = [&](int turns) {
            Reply window;
            backend.transcriptWindow(sessionId, generation, {{"turn_limit", 3}}, window.callback());
            if (!QTest::qWaitFor([&] { return window.done(); }, patience) ||
                !window.error.isEmpty())
                return false;
            int completed = 0;
            for (const QJsonValue& event : window.result->value("events").toArray())
                completed += event.toObject().value("event_type").toString() == "turn.completed";
            return completed >= turns;
        };
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(1), patience);
        // Files and images reach the real daemon through the SSH tunnel, with the
        // same bounded stage protocol used by Relay; no provider quota is involved.
        QVERIFY(backend.supports(ketplus::remote::MachineBackend::Feature::Attachments));
        const QList<QPair<QString, QByteArray>> attachments{
            {"text/plain", QByteArray("SSH attachment fixture\n")},
            {"image/png", QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC"
                                                 "0lEQVR42mP8/x8AAwMCAO+aWQ0AAAAASUVORK5CYII=")}};
        QJsonArray attachmentIds;
        for (const auto& attachment : attachments) {
            ketplus::remote::MachineAttachmentUpload upload(&backend, sessionId, generation);
            QSignalSpy finished(&upload, &ketplus::remote::MachineAttachmentUpload::finished);
            upload.start(attachment.first == "text/plain" ? "note.txt" : "image.png",
                         attachment.first, attachment.second);
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, patience);
            QVERIFY2(upload.ready(), qPrintable(finished[0][1].toString()));
            attachmentIds.append(upload.attachmentId());
        }
        // The second turn consumes the committed attachment ids.
        Reply approving;
        backend.startTurn(sessionId, generation,
                          {{"prompt", "approve"}, {"attachment_ids", attachmentIds}},
                          approving.callback());
        QTRY_VERIFY_WITH_TIMEOUT(approving.done(), patience);
        QVERIFY2(approving.error.isEmpty(), qPrintable(approving.error));

        QString approvalId;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                Reply pending;
                backend.pendingApprovals(sessionId, generation, {}, pending.callback());
                if (!QTest::qWaitFor([&] { return pending.done(); }, patience))
                    return false;
                const QJsonArray items = pending.result->value("pending_approvals").toArray();
                if (!items.isEmpty())
                    approvalId = items.first().toObject().value("approval_id").toString();
                return !approvalId.isEmpty();
            }(),
            patience);
        Reply resolved;
        backend.resolveApproval(sessionId, generation, approvalId, {{"decision", "accept"}},
                                resolved.callback());
        QTRY_VERIFY_WITH_TIMEOUT(resolved.done(), patience);
        QVERIFY2(resolved.error.isEmpty(), qPrintable(resolved.error));
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                Reply pending;
                backend.pendingApprovals(sessionId, generation, {}, pending.callback());
                return QTest::qWaitFor([&] { return pending.done(); }, patience) &&
                       pending.result->value("pending_approvals").toArray().isEmpty();
            }(),
            patience);

        // Paging: the newest window, then the turn before it.
        Reply window;
        backend.transcriptWindow(sessionId, generation, {{"turn_limit", 1}}, window.callback());
        QTRY_VERIFY_WITH_TIMEOUT(window.done(), patience);
        QVERIFY2(window.error.isEmpty(), qPrintable(window.error));
        QVERIFY(!window.result->value("events").toArray().isEmpty());
        Reply page;
        backend.transcriptPage(sessionId, generation, {{"before_turn_sequence", 2}, {"limit", 1}},
                               page.callback());
        QTRY_VERIFY_WITH_TIMEOUT(page.done(), patience);
        QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
        QCOMPARE(page.result->value("session_id").toString(), sessionId);
        QVERIFY(!page.result->value("events").toArray().isEmpty());

        // A restart of Desktop: the same daemon key is the same machine, and it reconnects.
        SshMachineStore reloaded(fixture.path("machines.json"));
        QCOMPARE(reloaded.machineId("fixture"), backend.machineId());
        QVERIFY(!tunnel->daemonCredential().isEmpty());
        backend.disconnectFromHost();
        QCOMPARE(backend.connectionState(), State::Offline);
        QVERIFY(tunnel->daemonCredential().isEmpty());
        SshMachineBackend again(target, &reloaded,
                                std::make_unique<SshTunnelLink>(target, fixture.options()));
        again.connectToHost();
        QTRY_COMPARE_WITH_TIMEOUT(again.connectionState(), State::Connected, patience);
        QCOMPARE(again.machineId(), backend.machineId());
        QVERIFY(conversationListed(again));
    }

    void aHostWithoutTheDaemonIsReported() {
        SshFixture fixture;
        if (!fixture.start(true, false))
            QSKIP(qPrintable(fixture.skip));
        fixture.trust(fixture.hostKey);
        SshMachineStore store(fixture.path("machines.json"));
        const SshTarget target{"fixture", {}, 0};
        QVERIFY(store.add(target));
        SshMachineBackend backend(target, &store,
                                  std::make_unique<SshTunnelLink>(target, fixture.options()));
        backend.connectToHost();
        QTRY_COMPARE_WITH_TIMEOUT(backend.tunnelState(), SshTunnel::State::DaemonSocketMissing,
                                  patience);
        QCOMPARE(backend.connectionState(), State::Offline);
        QVERIFY(backend.connectionReason().contains("daemon not installed"));
    }
};

QTEST_MAIN(SshDaemonIntegrationTest)
#include "SshDaemonIntegrationTest.moc"
