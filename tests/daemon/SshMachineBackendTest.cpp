#include "ssh/SshMachineBackend.h"
#include "FakeSshLink.h"
#include "ssh/KnownHostsFile.h"
#include "ssh/SshMachineStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using ketplus::remote::MachineBackend;
using ketplus::remote::ssh::hasKnownHost;
using ketplus::remote::ssh::HostKey;
using ketplus::remote::ssh::parseSshDestination;
using ketplus::remote::ssh::recordHostKey;
using ketplus::remote::ssh::SshLink;
using ketplus::remote::ssh::SshMachineBackend;
using ketplus::remote::ssh::SshMachineStore;
using ketplus::remote::ssh::SshTarget;
using ketplus::remote::ssh::sshTargetKey;
using ketplus::remote::ssh::SshTunnel;
using State = MachineBackend::ConnectionState;
using ketplus::remote::ssh::testing::FakeDaemon;
using ketplus::remote::ssh::testing::FakeLink;
using ketplus::remote::ssh::testing::token;

namespace {

struct Fixture {
    Fixture()
        : name(QStringLiteral("ketplus-ssh-backend-%1").arg(QCoreApplication::applicationPid())),
          daemon(name) {}

    [[nodiscard]] QString storePath() const { return directory.filePath("machines.json"); }

    std::unique_ptr<SshMachineBackend> backend(SshMachineStore& store, FakeLink** link = nullptr) {
        auto owned = std::make_unique<FakeLink>(name);
        if (link)
            *link = owned.get();
        return std::make_unique<SshMachineBackend>(target, &store, std::move(owned));
    }

    QTemporaryDir directory;
    QString name;
    FakeDaemon daemon;
    SshTarget target{QStringLiteral("devbox"), QStringLiteral("dev"), 2222};
};

} // namespace

class SshMachineBackendTest final : public QObject {
    Q_OBJECT

  private slots:
    void parsesConnectionsDestinations() {
        QString error;
        SshTarget target = parseSshDestination(QStringLiteral(" dev@devbox:2222 "), &error);
        QVERIFY2(target.isValid(), qPrintable(error));
        QCOMPARE(target.user, QStringLiteral("dev"));
        QCOMPARE(target.alias, QStringLiteral("devbox"));
        QCOMPARE(target.port, 2222);
        QCOMPARE(sshTargetKey(target), QStringLiteral("dev@devbox:2222"));

        target = parseSshDestination(QStringLiteral("devbox"));
        QVERIFY(target.isValid());
        QVERIFY(target.user.isEmpty());
        QCOMPARE(target.port, 0);
        QCOMPARE(sshTargetKey(target), QStringLiteral("devbox"));

        target = parseSshDestination(QStringLiteral("dev@[fd00::1]:22"));
        QCOMPARE(target.alias, QStringLiteral("fd00::1"));
        QCOMPARE(target.port, 22);

        for (const char* invalid : {"", "@devbox", "dev@devbox:0", "dev@devbox:99999",
                                    "dev@devbox:ssh", "-oProxyCommand=x", "dev@[fd00::1"}) {
            error.clear();
            QVERIFY2(!parseSshDestination(QString::fromLatin1(invalid), &error).isValid(), invalid);
            QVERIFY(!error.isEmpty());
        }
    }

    void mapsTunnelStatesToConnectionStates() {
        Fixture fixture;
        SshMachineStore store(fixture.storePath());
        QVERIFY(store.add(fixture.target));
        FakeLink* link = nullptr;
        auto backend = fixture.backend(store, &link);
        link->autoConnect = false;
        QCOMPARE(backend->route(), MachineBackend::Route::Ssh);
        QCOMPARE(backend->connectionState(), State::Connecting);

        const struct {
            SshTunnel::State tunnel;
            State expected;
            const char* reason;
        } cases[] = {
            {SshTunnel::State::Connecting, State::Connecting, ""},
            {SshTunnel::State::Offline, State::Offline, "cannot be reached"},
            {SshTunnel::State::AuthFailed, State::Offline, "refused the sign-in"},
            {SshTunnel::State::HostKeyMismatch, State::Offline, "SSH key changed"},
            {SshTunnel::State::HostKeyRejected, State::Offline, "not trusted"},
            {SshTunnel::State::DaemonSocketMissing, State::Offline, "daemon not installed"},
            {SshTunnel::State::DaemonTokenInsecure, State::Offline, "readable by other users"},
            // Forwarded but no hello yet: still connecting.
            {SshTunnel::State::Connected, State::Connecting, ""},
        };
        for (const auto& entry : cases) {
            link->setTunnelState(entry.tunnel);
            QCOMPARE(backend->connectionState(), entry.expected);
            QVERIFY2(backend->connectionReason().contains(QString::fromLatin1(entry.reason)),
                     qPrintable(backend->connectionReason()));
        }

        // Offline does not retry for the caller: a request fails at once with the reason.
        link->setTunnelState(SshTunnel::State::DaemonSocketMissing);
        QString failure;
        bool answered = false;
        backend->listRepositories([&](const QJsonObject&, const QString& error) {
            answered = true;
            failure = error;
        });
        QVERIFY(answered);
        QCOMPARE(failure, SshMachineBackend::reasonFor(SshTunnel::State::DaemonSocketMissing));
        QVERIFY(fixture.daemon.requests.isEmpty());
    }

    void firstHelloNamesTheMachineAndRequestsFlow() {
        Fixture fixture;
        {
            SshMachineStore store(fixture.storePath());
            QVERIFY(store.add(fixture.target));
            auto backend = fixture.backend(store);
            QVERIFY(backend->machineId().isEmpty());
            QSignalSpy named(backend.get(), &SshMachineBackend::machineIdChanged);

            // Made while connecting: queued until the hello, then answered.
            QJsonObject repositories;
            backend->listRepositories(
                [&](const QJsonObject& result, const QString&) { repositories = result; });
            backend->connectToHost();
            QTRY_COMPARE(backend->connectionState(), State::Connected);
            QTRY_COMPARE(repositories.value("repositories").toArray().size(), 1);
            QCOMPARE(backend->machineId(), QStringLiteral("daemon-key-A"));
            QCOMPARE(named.size(), 1);

            const QJsonObject details = backend->connectionDetails();
            QCOMPARE(details.value("route").toString(), QStringLiteral("SSH"));
            QCOMPARE(details.value("target").toString(), QStringLiteral("dev@devbox:2222"));
            QCOMPARE(details.value("machine_id").toString(), QStringLiteral("daemon-key-A"));
            QCOMPARE(details.value("ssh_host_key_fingerprint").toString(),
                     QStringLiteral("SHA256:sshkey"));
            QCOMPARE(details.value("state").toString(), QStringLiteral("connected"));
            QVERIFY(!QJsonDocument(details).toJson().contains(token));
            QVERIFY(backend->bindConversation(QStringLiteral("conversation-1")));
        }

        // A restart: the id, the target and the binding come back; no secret was written.
        QFile file(fixture.storePath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        QVERIFY(!saved.contains(token));
        QCOMPARE(QFileInfo(fixture.storePath()).permissions() &
                     (QFileDevice::ReadGroup | QFileDevice::ReadOther),
                 QFileDevice::Permissions{});
        SshMachineStore reloaded(fixture.storePath());
        QCOMPARE(reloaded.machines().size(), 1);
        QCOMPARE(reloaded.machineId(sshTargetKey(fixture.target)), QStringLiteral("daemon-key-A"));
        QCOMPARE(reloaded.conversationMachine(QStringLiteral("conversation-1")),
                 QStringLiteral("daemon-key-A"));

        // Reconnecting after the restart presents the same daemon key and connects.
        auto backend = fixture.backend(reloaded);
        QCOMPARE(backend->machineId(), QStringLiteral("daemon-key-A"));
        backend->connectToHost();
        QTRY_COMPARE(backend->connectionState(), State::Connected);
    }

    void createSessionStartsTheFirstTurnInTheRemoteWorktree() {
        Fixture fixture;
        SshMachineStore store(fixture.storePath());
        QVERIFY(store.add(fixture.target));
        auto backend = fixture.backend(store);
        backend->connectToHost();
        QTRY_COMPARE(backend->connectionState(), State::Connected);
        bool created = false;
        backend->createSession({{"repository_id", "/home/dev/app"},
                                {"worktree_id", "/home/dev/app"},
                                {"prompt", "hello"},
                                {"options", QJsonObject{{"model", "fixture"}}}},
                               [&](const QJsonObject& result, const QString& error) {
                                   created = error.isEmpty() && result.value("ok").toBool();
                               });
        QTRY_VERIFY(created);
        QCOMPARE(fixture.daemon.requests.last(), QStringLiteral("session.start_turn"));
        const auto payload = fixture.daemon.payloads.last();
        QCOMPARE(payload.value("worktree_path").toString(), QStringLiteral("/home/dev/app"));
        QCOMPARE(payload.value("prompt").toString(), QStringLiteral("hello"));
        QCOMPARE(payload.value("options").toObject().value("model").toString(),
                 QStringLiteral("fixture"));
    }

    // T-18: fork and archive follow the remote daemon's hello; rename and the others forward
    // to the daemon's local protocol.
    void conversationOperationsFollowTheDaemonsHello() {
        using Feature = MachineBackend::Feature;
        Fixture fixture;
        SshMachineStore store(fixture.storePath());
        QVERIFY(store.add(fixture.target));
        auto backend = fixture.backend(store);
        QVERIFY(!backend->supports(Feature::Rename)); // not connected yet
        QCOMPARE(backend->unsupportedReason(Feature::Rename),
                 QStringLiteral("Available once this machine is connected."));
        backend->connectToHost();
        QTRY_COMPARE(backend->connectionState(), State::Connected);
        // An older daemon: no session.fork, no session.archive/1.
        QVERIFY(backend->supports(Feature::Rename));
        QVERIFY(!backend->supports(Feature::Fork));
        QVERIFY(!backend->supports(Feature::Archive));
        QVERIFY(!backend->supports(Feature::Attachments));
        QCOMPARE(backend->unsupportedReason(Feature::Fork),
                 QStringLiteral("Not available on Linux machines yet."));
        QString refused;
        backend->forkConversation(
            "conversation-1", 1, "fork-1",
            [&](const QJsonObject&, const QString& error) { refused = error; });
        QTRY_VERIFY(!refused.isEmpty());
        QVERIFY(!fixture.daemon.requests.contains("session.fork"));

        bool renamed = false;
        backend->renameConversation(
            "conversation-1", 1, "Release notes",
            [&](const QJsonObject&, const QString& error) { renamed = error.isEmpty(); });
        QTRY_VERIFY(renamed);
        QCOMPARE(fixture.daemon.requests.last(), QStringLiteral("session.rename"));
        QCOMPARE(fixture.daemon.payloads.last().value("title").toString(),
                 QStringLiteral("Release notes"));
    }

    // A daemon that offers fork and archive: both forward.
    void forkAndArchiveForwardWhenTheDaemonOffersThem() {
        using Feature = MachineBackend::Feature;
        Fixture current;
        current.daemon.features = {"daemon.host_identity/1", "session.fork", "session.archive/1"};
        SshMachineStore currentStore(current.storePath());
        QVERIFY(currentStore.add(current.target));
        auto newer = current.backend(currentStore);
        newer->connectToHost();
        QTRY_COMPARE(newer->connectionState(), State::Connected);
        QVERIFY(newer->supports(Feature::Fork));
        QVERIFY(newer->supports(Feature::Archive));
        bool archived = false;
        newer->setConversationArchived(
            "conversation-1", 1, true,
            [&](const QJsonObject&, const QString& error) { archived = error.isEmpty(); });
        QTRY_VERIFY(archived);
        QCOMPARE(current.daemon.requests.last(), QStringLiteral("session.set_archived"));
        QCOMPARE(current.daemon.payloads.last().value("archived").toBool(), true);
        bool forked = false;
        newer->forkConversation(
            "conversation-1", 1, "fork-1",
            [&](const QJsonObject&, const QString& error) { forked = error.isEmpty(); });
        QTRY_VERIFY(forked);
        QCOMPARE(current.daemon.requests.last(), QStringLiteral("session.fork"));
        QCOMPARE(current.daemon.payloads.last().value("fork_session_id").toString(),
                 QStringLiteral("fork-1"));
    }

    void aDroppedSessionReconnectsAndReplaysReads() {
        Fixture fixture;
        SshMachineStore store(fixture.storePath());
        QVERIFY(store.add(fixture.target));
        FakeLink* link = nullptr;
        auto backend = fixture.backend(store, &link);
        backend->connectToHost();
        QTRY_COMPARE(backend->connectionState(), State::Connected);

        link->dropConnection();
        QTRY_COMPARE(backend->connectionState(), State::Reconnecting);
        bool listed = false;
        backend->listConversations({}, [&](const QJsonObject& result, const QString& error) {
            listed = error.isEmpty() && result.value("ok").toBool();
        });
        // The tunnel retries on its own; the next connection carries the queued read.
        link->open();
        QTRY_COMPARE(backend->connectionState(), State::Connected);
        QTRY_VERIFY(listed);
        QCOMPARE(fixture.daemon.hellos, 2);
    }

    void aChangedDaemonKeyFailsClosedUntilTrustedAndMovesTheBindings() {
        Fixture fixture;
        const QString key = sshTargetKey(fixture.target);
        {
            SshMachineStore store(fixture.storePath());
            QVERIFY(store.add(fixture.target));
            QVERIFY(store.trustFirstIdentity(key, QStringLiteral("daemon-key-A")));
            QVERIFY(store.bindConversation(QStringLiteral("conversation-1"),
                                           QStringLiteral("daemon-key-A")));
            QVERIFY(store.bindConversation(QStringLiteral("elsewhere"),
                                           QStringLiteral("another-machine")));
        }
        // The host's daemon lost its credential store and made a new key.
        fixture.daemon.fingerprint = QStringLiteral("daemon-key-B");
        SshMachineStore store(fixture.storePath());
        FakeLink* link = nullptr;
        auto backend = fixture.backend(store, &link);
        QString failure;
        backend->listRepositories(
            [&](const QJsonObject&, const QString& error) { failure = error; });
        backend->connectToHost();
        QTRY_VERIFY(backend->identityChanged());
        QCOMPARE(backend->connectionState(), State::Offline);
        QCOMPARE(backend->connectionReason(),
                 ketplus::remote::DaemonClient::identityRejectedReason());
        QCOMPARE(backend->presentedMachineId(), QStringLiteral("daemon-key-B"));
        QCOMPARE(backend->machineId(), QStringLiteral("daemon-key-A"));
        QCOMPARE(failure, ketplus::remote::DaemonClient::identityRejectedReason());
        QTRY_COMPARE(link->state(), SshTunnel::State::Idle);
        QCOMPARE(backend->connectionDetails().value("presented_machine_id").toString(),
                 QStringLiteral("daemon-key-B"));

        // Nothing reached the new daemon, and Connect does not get around the check.
        const int opens = link->opens;
        backend->connectToHost();
        QTest::qWait(200);
        QCOMPARE(link->opens, opens);
        QCOMPARE(backend->connectionState(), State::Offline);
        QCOMPARE(fixture.daemon.hellos, 1);
        QVERIFY(fixture.daemon.requests.isEmpty());
        QCOMPARE(SshMachineStore(fixture.storePath()).machineId(key),
                 QStringLiteral("daemon-key-A"));

        // Explicit re-trust: the id and its conversations move in one write, then it connects.
        QSignalSpy named(backend.get(), &SshMachineBackend::machineIdChanged);
        QVERIFY(backend->trustPresentedIdentity());
        QCOMPARE(named.size(), 1);
        QCOMPARE(backend->machineId(), QStringLiteral("daemon-key-B"));
        QTRY_COMPARE(backend->connectionState(), State::Connected);
        bool listed = false;
        backend->listRepositories(
            [&](const QJsonObject&, const QString& error) { listed = error.isEmpty(); });
        QTRY_VERIFY(listed);

        SshMachineStore reloaded(fixture.storePath());
        QCOMPARE(reloaded.machineId(key), QStringLiteral("daemon-key-B"));
        QCOMPARE(reloaded.conversationMachine(QStringLiteral("conversation-1")),
                 QStringLiteral("daemon-key-B"));
        QCOMPARE(reloaded.conversationMachine(QStringLiteral("elsewhere")),
                 QStringLiteral("another-machine"));
    }

    void aDaemonThatNamesNoIdentityIsRefused() {
        Fixture fixture;
        fixture.daemon.fingerprint.clear();
        SshMachineStore store(fixture.storePath());
        QVERIFY(store.add(fixture.target));
        auto backend = fixture.backend(store);
        backend->connectToHost();
        QTRY_VERIFY(backend->identityChanged());
        QCOMPARE(backend->connectionState(), State::Offline);
        QVERIFY(backend->connectionReason().contains(QStringLiteral("does not name its identity")));
        QVERIFY(backend->machineId().isEmpty());
        QVERIFY(!backend->trustPresentedIdentity());
        QVERIFY(
            SshMachineStore(fixture.storePath()).machineId(sshTargetKey(fixture.target)).isEmpty());
        QVERIFY(fixture.daemon.requests.isEmpty());

        // The host's daemon is updated: Connect tries again and names the machine.
        fixture.daemon.fingerprint = QStringLiteral("daemon-key-A");
        backend->connectToHost();
        QTRY_COMPARE(backend->connectionState(), State::Connected);
        QVERIFY(!backend->identityChanged());
        QCOMPARE(backend->machineId(), QStringLiteral("daemon-key-A"));
        QCOMPARE(SshMachineStore(fixture.storePath()).machineId(sshTargetKey(fixture.target)),
                 QStringLiteral("daemon-key-A"));
    }

    void aManualReconnectIsAFirstAttemptAgain() {
        Fixture fixture;
        SshMachineStore store(fixture.storePath());
        QVERIFY(store.add(fixture.target));
        FakeLink* link = nullptr;
        auto backend = fixture.backend(store, &link);
        backend->connectToHost();
        QTRY_COMPARE(backend->connectionState(), State::Connected);
        backend->disconnectFromHost();
        QCOMPARE(backend->connectionState(), State::Offline);
        link->autoConnect = false;
        backend->connectToHost();
        QCOMPARE(backend->connectionState(), State::Connecting);
        link->setTunnelState(SshTunnel::State::Connecting);
        QCOMPARE(backend->connectionState(), State::Connecting);
        // A host never reached in this attempt is Offline, not Reconnecting.
        link->setTunnelState(SshTunnel::State::Offline);
        QCOMPARE(backend->connectionState(), State::Offline);
    }

    void aMismatchedSshKeyIsNotShownAsVerified() {
        Fixture fixture;
        SshMachineStore store(fixture.storePath());
        QVERIFY(store.add(fixture.target));
        FakeLink* link = nullptr;
        auto backend = fixture.backend(store, &link);
        link->autoConnect = false;
        link->setTunnelState(SshTunnel::State::HostKeyMismatch);
        QVERIFY(backend->sshHostKeyFingerprint().isEmpty());
        QCOMPARE(backend->presentedSshHostKeyFingerprint(), QStringLiteral("SHA256:sshkey"));
        const QJsonObject details = backend->connectionDetails();
        QVERIFY(details.value("ssh_host_key_fingerprint").toString().isEmpty());
        QCOMPARE(details.value("presented_ssh_host_key_fingerprint").toString(),
                 QStringLiteral("SHA256:sshkey"));
    }

    void addingAndRemovingMachinesIsDurable() {
        Fixture fixture;
        const SshTarget other{QStringLiteral("buildbox"), {}, 0};
        {
            SshMachineStore store(fixture.storePath());
            QVERIFY(store.add(fixture.target));
            QVERIFY(store.add(other));
            QVERIFY(store.add(fixture.target)); // known: kept once
            QVERIFY(!store.add(SshTarget{QStringLiteral("-oProxyCommand=x"), {}, 0}));
        }
        SshMachineStore store(fixture.storePath());
        QCOMPARE(store.machines().size(), 2);

        auto backend = fixture.backend(store);
        backend->connectToHost();
        QTRY_COMPARE(backend->connectionState(), State::Connected);
        QVERIFY(backend->bindConversation(QStringLiteral("conversation-1")));
        QVERIFY(backend->removeMachine());
        QCOMPARE(backend->connectionState(), State::Offline);
        QCOMPARE(backend->connectionReason(), QStringLiteral("Disconnected."));

        SshMachineStore reloaded(fixture.storePath());
        QCOMPARE(reloaded.machines().size(), 1);
        QCOMPARE(sshTargetKey(reloaded.machines().first().target), QStringLiteral("buildbox"));
        QVERIFY(reloaded.conversationMachine(QStringLiteral("conversation-1")).isEmpty());
    }

    void acceptedHostKeysAreForgottenAndRemovedWithTheMachine() {
        Fixture fixture;
        const QString knownHosts = fixture.directory.filePath("known_hosts");
        const SshTarget other{QStringLiteral("buildbox"), {}, 0};
        SshMachineStore store(fixture.storePath());
        QVERIFY(store.add(fixture.target));
        QVERIFY(store.add(other));
        QVERIFY(recordHostKey(knownHosts,
                              HostKey{QStringLiteral("buildbox"), QStringLiteral("ssh-ed25519"),
                                      QStringLiteral("AAAAC3NzaC1lZDI1NTE5AAAAIB")}));
        FakeLink* link = nullptr;
        auto backend = fixture.backend(store, &link);
        link->knownHosts = knownHosts;
        QStringList asked;
        backend->setHostKeyPrompt([&asked](const HostKey& key, std::function<void(bool)> decide) {
            asked.append(key.knownHostsName);
            decide(true);
        });
        QVERIFY(!backend->holdsAcceptedHostKey());
        // A key the user accepted in ketPlus is remembered with the machine.
        link->offerKey(HostKey{QStringLiteral("[devbox]:2222"), QStringLiteral("ssh-ed25519"),
                               QStringLiteral("AAAAC3NzaC1lZDI1NTE5AAAAIA")});
        QCOMPARE(asked, QStringList{QStringLiteral("[devbox]:2222")});
        QVERIFY(hasKnownHost(knownHosts, QStringLiteral("[devbox]:2222")));
        QVERIFY(backend->holdsAcceptedHostKey());
        QCOMPARE(
            SshMachineStore(fixture.storePath()).acceptedHostKeys(sshTargetKey(fixture.target)),
            QStringList{QStringLiteral("[devbox]:2222")});

        // Re-trust after a mismatch: the accepted key goes, so the next connect asks again.
        QVERIFY(backend->forgetAcceptedHostKeys());
        QVERIFY(!hasKnownHost(knownHosts, QStringLiteral("[devbox]:2222")));
        QVERIFY(!backend->forgetAcceptedHostKeys()); // nothing of ketPlus's left: user's file
        link->offerKey(HostKey{QStringLiteral("[devbox]:2222"), QStringLiteral("ssh-ed25519"),
                               QStringLiteral("AAAAC3NzaC1lZDI1NTE5AAAAIC")});
        QVERIFY(hasKnownHost(knownHosts, QStringLiteral("[devbox]:2222")));

        // Removing the machine removes its key and leaves the other machine's key alone.
        QVERIFY(backend->removeMachine());
        QVERIFY(!hasKnownHost(knownHosts, QStringLiteral("[devbox]:2222")));
        QVERIFY(hasKnownHost(knownHosts, QStringLiteral("buildbox")));
        QVERIFY(!SshMachineStore(fixture.storePath()).contains(sshTargetKey(fixture.target)));
    }

    void aRejectedHostKeyIsNotRemembered() {
        Fixture fixture;
        SshMachineStore store(fixture.storePath());
        QVERIFY(store.add(fixture.target));
        FakeLink* link = nullptr;
        auto backend = fixture.backend(store, &link);
        link->knownHosts = fixture.directory.filePath("known_hosts");
        backend->setHostKeyPrompt(
            [](const HostKey&, std::function<void(bool)> decide) { decide(false); });
        link->offerKey(HostKey{QStringLiteral("[devbox]:2222"), QStringLiteral("ssh-ed25519"),
                               QStringLiteral("AAAAC3NzaC1lZDI1NTE5AAAAIA")});
        QVERIFY(store.acceptedHostKeys(sshTargetKey(fixture.target)).isEmpty());
        QVERIFY(!QFile::exists(link->knownHosts));
    }

    void anUnreadableStoreIsNeverOverwritten() {
        QTemporaryDir directory;
        const QString path = directory.filePath("machines.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"version": 99, "machines": []})");
        file.close();
        SshMachineStore store(path);
        QVERIFY(!store.isUsable());
        QString error;
        QVERIFY(!store.add(SshTarget{QStringLiteral("devbox"), {}, 0}, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("99"));
    }
};

QTEST_MAIN(SshMachineBackendTest)
#include "SshMachineBackendTest.moc"
