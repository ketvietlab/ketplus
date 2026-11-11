// ADR 0116: team commands from paired phones at the cloud layer. The associated data matches
// the contract's vectors, the result opens only with the requesting phone's key and carries
// no team content the relay can read, the scope and the pairing are checked, changes are
// audited with their team, and the heartbeat advertises the capability. The relay is an
// in-memory fake; credentials are in memory; no real provider is started.
#include "AgentTestFixture.h"
#include "cloud-privacy/CloudPrivacyTestAccess.h"
#include "cloud-privacy/FakeTransport.h"
#include "cloud-privacy/MemoryCredentials.h"
#include "cloud/CloudCrypto.h"
#include "cloud/RemoteAudit.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>

using namespace agent_test;
using Access = ketplus::daemon::CloudPrivacyTestAccess;
using ketplus::daemon::CloudControlClient;
using ketplus::daemon::CloudCrypto;
using ketplus::daemon::CredentialStore;

namespace {

QJsonObject contract() {
    QFile file(QStringLiteral(KETPLUS_TEST_FIXTURES "/team-remote-v1.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

void git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", arguments);
    QVERIFY(process.waitForFinished(15000));
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
}

// As the relay hands a claimed command to the daemon.
QJsonObject claimed(QJsonObject request, const QString& owner) {
    request.insert("sessionId", "");
    request.insert("sessionGeneration", 0);
    request.insert("ownerSubjectId", owner);
    request.insert("attempt", 1);
    return request;
}

struct Relay final {
    Fixture fixture;
    r03::Transport transport;
    std::unique_ptr<CloudControlClient> client;
    CredentialStore phoneStore{"team-phone"};
    QJsonObject phoneKeys;
    QString team;
    QString developer;

    bool open() {
        r03::resetCredentials();
        if (!fixture.open())
            return false;
        client = std::make_unique<CloudControlClient>(fixture.journal.get(), fixture.host.get());
        Access::configure(*client, transport);
        Access::machineKeyVersion(*client, 1);
        phoneKeys = CloudCrypto(&phoneStore).publicKeys();
        return !phoneKeys.isEmpty();
    }

    QByteArray phoneAgreementKey() const {
        return CloudCrypto::fromBase64Url(phoneKeys.value("agreementPublicKey").toString());
    }

    QJsonObject command(const QString& id, const QString& kind, const QString& capability) const {
        return {{"id", id},
                {"kind", kind},
                {"capability", capability},
                {"attempt", 1},
                {"idempotencyKey", "phone-action-" + id},
                {"workspaceId", "workspace-synthetic"},
                {"machineId", "machine-synthetic"},
                {"sessionId", ""},
                {"sessionGeneration", 0},
                {"ownerSubjectId", "owner-synthetic"},
                {"content", QJsonObject{{"recipientDeviceId", "machine-synthetic"},
                                        {"recipientKeyVersion", 1},
                                        {"senderDeviceId", "team-phone"},
                                        {"senderKeyVersion", 1}}}};
    }

    // The acknowledgement the relay received for `id`, or an empty object.
    QJsonObject acknowledgement(const QString& id) const {
        const QString path = QStringLiteral("/v2/commands/%1/ack").arg(id);
        for (qsizetype index = 0; index < transport.requests.size(); ++index)
            if (transport.requests.at(index).url().path() == path)
                return QJsonDocument::fromJson(transport.bodies.at(index)).object();
        return {};
    }

    QByteArray acknowledgementBytes(const QString& id) const {
        const QString path = QStringLiteral("/v2/commands/%1/ack").arg(id);
        for (qsizetype index = 0; index < transport.requests.size(); ++index)
            if (transport.requests.at(index).url().path() == path)
                return transport.bodies.at(index);
        return {};
    }

    // Runs the command and opens its result as the phone does.
    QJsonObject run(const QJsonObject& command, const QJsonObject& payload) {
        const QString id = command.value("id").toString();
        Access::teamCommand(*client, command, payload, phoneAgreementKey());
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!acknowledgement(id).isEmpty(), 10000); }();
        const QJsonObject ack = acknowledgement(id);
        const QJsonObject sealed = ack.value("encryptedResult").toObject();
        const QJsonObject shell{{"keyId", sealed.value("keyId")},
                                {"keyEpoch", sealed.value("keyEpoch")},
                                {"senderDeviceId", sealed.value("senderDeviceId")},
                                {"senderKeyVersion", sealed.value("senderKeyVersion")}};
        const QByteArray aad =
            Access::resultAad(*client, command, 1, ack.value("status").toString(),
                              ack.value("resultCode").toString(), shell);
        const QByteArray macSigning = CloudCrypto::fromBase64Url(
            Access::crypto(*client).publicKeys().value("signingPublicKey").toString());
        QString error;
        const QByteArray plaintext =
            CloudCrypto(&phoneStore)
                .decryptSealedAs(sealed, aad, macSigning, id, "team-result:" + id, &error);
        return QJsonDocument::fromJson(plaintext).object();
    }

    bool createTeam() {
        const QString repository = fixture.directory.filePath("shop");
        QDir().mkpath(repository);
        git(repository, {"init", "-q", "-b", "integration"});
        git(repository, {"-c", "user.email=t@example.com", "-c", "user.name=T", "commit", "-q",
                         "--allow-empty", "-m", "start"});
        const QJsonObject created = fixture.host->handleCommand(
            request("team.create",
                    {{"repository", repository},
                     {"name", "Secret shop"},
                     {"goal", "A goal the relay never reads"},
                     {"integration_branch", "integration"},
                     {"plan",
                      QJsonObject{{"leader", QJsonObject{{"provider", "claude"}}},
                                  {"developers", QJsonObject{{"count", 1}, {"provider", "codex"}}},
                                  {"reviewer", QJsonObject{{"provider", "claude"}}}}}}),
            "desktop-test");
        team = created.value("team").toObject().value("id").toString();
        for (const QJsonValue& value : created.value("team").toObject().value("members").toArray())
            if (value.toObject().value("role") == QJsonValue("developer"))
                developer = value.toObject().value("session_id").toString();
        return !team.isEmpty();
    }

    qint64 revision() const {
        return fixture.host->teamMetadata().value(developer).value("revision").toInteger();
    }
};

} // namespace

class TeamRemoteCloudTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // §2: the associated data of a team command and of its result, as the contract's vectors.
    void theAssociatedDataMatchesTheContract() {
        const QJsonObject fixtureJson = contract();
        QVERIFY(!fixtureJson.isEmpty());
        Relay relay;
        QVERIFY(relay.open());
        for (const QString& name :
             {QStringLiteral("snapshot"), QStringLiteral("answeredDecision"),
              QStringLiteral("refusal"), QStringLiteral("createAssociatedData")}) {
            const QJsonObject entry = fixtureJson.value(name).toObject();
            const QJsonObject command = claimed(entry.value("request").toObject(),
                                                entry.value("ownerSubjectId").toString());
            QCOMPARE(QString::fromUtf8(Access::commandAad(*relay.client, command)),
                     entry.value("associatedData").toString());
            const QJsonObject sealed = entry.value("sealedResult").toObject();
            if (sealed.isEmpty())
                continue;
            const QString code =
                entry.value("resultCode").toString(name == "snapshot" ? "ok" : QString{});
            const QString status = entry.value("status").toString(
                code == QStringLiteral("ok") ? "completed" : "rejected");
            QCOMPARE(QString::fromUtf8(Access::resultAad(
                         *relay.client, command, 1, status, code,
                         {{"keyId", sealed.value("keyId")},
                          {"keyEpoch", sealed.value("keyEpoch")},
                          {"senderDeviceId", sealed.value("senderDeviceId")},
                          {"senderKeyVersion", sealed.value("senderKeyVersion")}})),
                     sealed.value("associatedData").toString());
        }
    }

    // §2, §5: the result is sealed to the phone that asked; the relay reads only the outcome.
    void theResultOpensOnlyForThePhoneAndHidesTheTeam() {
        Relay relay;
        QVERIFY(relay.open());
        QVERIFY(relay.createTeam());
        const QJsonObject command = relay.command("team-read-1", "team.list", "teams.read");
        const QJsonObject result = relay.run(command, {});
        QVERIFY2(result.value("ok").toBool(), qPrintable(text(result)));
        QCOMPARE(result.value("teams").toArray().first().toObject().value("name"),
                 QJsonValue("Secret shop"));
        const QJsonObject sealed =
            relay.acknowledgement("team-read-1").value("encryptedResult").toObject();
        QCOMPARE(sealed.value("keyId"), QJsonValue("team-result:team-read-1"));
        QCOMPARE(sealed.value("recipientDeviceId"), QJsonValue("team-phone"));
        QCOMPARE(sealed.value("senderDeviceId"), QJsonValue("machine-synthetic"));
        // Another device's key does not open it.
        CredentialStore otherStore("other-phone");
        (void)CloudCrypto(&otherStore).publicKeys();
        QVERIFY(CloudCrypto(&otherStore)
                    .decryptSealedAs(sealed, "x", "x", "team-read-1", "team-result:team-read-1")
                    .isEmpty());

        // A change: what the relay receives holds no team content.
        const QJsonObject change = relay.command("team-write-1", "team.update", "teams.write");
        const QJsonObject paused = relay.run(
            change,
            {{"team_id", relay.team}, {"paused", true}, {"expected_revision", relay.revision()}});
        QVERIFY2(paused.value("ok").toBool(), qPrintable(text(paused)));
        for (const QString& id : {QStringLiteral("team-read-1"), QStringLiteral("team-write-1")}) {
            const QByteArray bytes = relay.acknowledgementBytes(id);
            QVERIFY(!bytes.isEmpty());
            for (const QByteArray& secret :
                 {QByteArray("Secret shop"), QByteArray("A goal the relay never reads"),
                  QByteArray("integration"), relay.developer.toUtf8(), QByteArray("\"team\""),
                  QByteArray("paused")})
                QVERIFY2(!bytes.contains(secret), secret.constData());
        }
    }

    // §5: a command outside this Mac's owner, capability or mailbox is refused, as is a
    // device that is not, or no longer, paired here.
    void anOutOfScopeOrUnpairedDeviceIsRefused() {
        Relay relay;
        QVERIFY(relay.open());
        QVERIFY(relay.createTeam());
        const QJsonObject payload{
            {"team_id", relay.team}, {"paused", true}, {"expected_revision", relay.revision()}};
        const auto code = [&](const QString& id, const std::function<void(QJsonObject&)>& change) {
            QJsonObject command = relay.command(id, "team.update", "teams.write");
            change(command);
            return relay.run(command, payload).value("error_code").toString();
        };
        QCOMPARE(code("scope-1", [](QJsonObject& c) { c.insert("capability", "teams.read"); }),
                 QStringLiteral("team_scope_forbidden"));
        QCOMPARE(code("scope-2", [](QJsonObject& c) { c.insert("ownerSubjectId", ""); }),
                 QStringLiteral("team_scope_forbidden"));
        QCOMPARE(code("scope-3",
                      [](QJsonObject& c) {
                          QJsonObject content = c.value("content").toObject();
                          content.insert("recipientKeyVersion", 2);
                          c.insert("content", content);
                      }),
                 QStringLiteral("team_scope_forbidden"));
        QCOMPARE(code("scope-4", [](QJsonObject& c) { c.insert("sessionId", "conversation"); }),
                 QStringLiteral("team_scope_forbidden"));
        QVERIFY(!relay.fixture.host
                     ->handleCommand(request("team.get", {{"team_id", relay.team}}), "desktop-test")
                     .value("team")
                     .toObject()
                     .value("paused")
                     .toBool());

        // A device this Mac never paired, or unpaired since, gets nothing done.
        QVERIFY(Access::pins(*relay.client).reset());
        relay.transport.statusForPath.insert("/v2/device-keys/team-phone/1", 200);
        relay.transport.responseForPath.insert(
            "/v2/device-keys/team-phone/1",
            QJsonDocument(QJsonObject{{"keys", relay.phoneKeys}}).toJson());
        QJsonObject unpaired = relay.command("unpaired-1", "team.update", "teams.write");
        Access::execute(*relay.client, unpaired);
        QTRY_VERIFY([&] {
            const auto audit = relay.fixture.journal->remoteCommands("team-phone", 10);
            return audit && !audit->isEmpty() &&
                   audit->first().resultCode == QStringLiteral("sender_key_unavailable");
        }());
        QVERIFY(relay.acknowledgement("unpaired-1").value("encryptedResult").toObject().isEmpty());
        QVERIFY(!relay.fixture.host
                     ->handleCommand(request("team.get", {{"team_id", relay.team}}), "desktop-test")
                     .value("team")
                     .toObject()
                     .value("paused")
                     .toBool());
    }

    // §5: every change is audited with its device, key version, result and team; reads are not.
    void changesAreAuditedWithTheirTeam() {
        Relay relay;
        QVERIFY(relay.open());
        QVERIFY(relay.createTeam());
        (void)relay.run(relay.command("audit-read", "team.get", "teams.read"),
                        {{"team_id", relay.team}});
        (void)relay.run(
            relay.command("audit-write", "team.update", "teams.write"),
            {{"team_id", relay.team}, {"paused", true}, {"expected_revision", relay.revision()}});
        (void)relay.run(relay.command("audit-stale", "team.update", "teams.write"),
                        {{"team_id", relay.team}, {"paused", false}, {"expected_revision", 0}});
        const auto audit = relay.fixture.journal->remoteCommands("team-phone", 10);
        QVERIFY(audit);
        QHash<QString, ketplus::daemon::RemoteCommandAudit> byId;
        for (const auto& entry : *audit)
            byId.insert(entry.commandId, entry);
        QVERIFY(!byId.contains("audit-read"));
        QCOMPARE(byId.value("audit-write").kind, QStringLiteral("team.update"));
        QCOMPARE(byId.value("audit-write").teamId, relay.team);
        QCOMPARE(byId.value("audit-write").deviceKeyVersion, 1);
        QCOMPARE(byId.value("audit-write").resultCode, QStringLiteral("ok"));
        QCOMPARE(byId.value("audit-stale").resultCode, QStringLiteral("stale_team"));
        const QJsonArray listed = ketplus::daemon::remoteAuditJson(*audit);
        QCOMPARE(listed.first().toObject().value("team_id"), QJsonValue(relay.team));
    }

    // §1, §3: the heartbeat advertises the capability; a team change asks for a heartbeat,
    // at most one every two seconds; the member's metadata carries its team.
    void theHeartbeatCarriesTeams() {
        Relay relay;
        QVERIFY(relay.open());
        Access::heartbeat(*relay.client, {QJsonArray{}});
        QTRY_VERIFY(std::any_of(
            relay.transport.requests.cbegin(), relay.transport.requests.cend(),
            [](const auto& request) { return request.url().path().endsWith("/heartbeat"); }));
        bool advertised = false;
        for (qsizetype index = 0; index < relay.transport.requests.size(); ++index)
            if (relay.transport.requests.at(index).url().path().endsWith("/heartbeat"))
                advertised = QJsonDocument::fromJson(relay.transport.bodies.at(index))
                                 .object()
                                 .value("capabilities")
                                 .toArray()
                                 .contains(QJsonValue("teams.remote/1"));
        QVERIFY(advertised);

        QVERIFY(relay.createTeam());
        QVERIFY(Access::teamSyncPending(*relay.client));
        QTRY_VERIFY_WITH_TIMEOUT(!Access::teamSyncPending(*relay.client), 3000);
        // A second change right after the heartbeat waits for the rest of the interval.
        (void)relay.fixture.host->handleCommand(
            request("team.update", {{"team_id", relay.team}, {"paused", true}}), "desktop-test");
        QVERIFY(Access::teamSyncPending(*relay.client));
        QTest::qWait(1000);
        QVERIFY(Access::teamSyncPending(*relay.client));

        const auto sessions = relay.fixture.journal->sessions();
        const auto member = std::find_if(sessions.cbegin(), sessions.cend(),
                                         [&](const auto& s) { return s.id == relay.developer; });
        QVERIFY(member != sessions.cend());
        const QJsonObject sealed = Access::metadata(
            *member, {{relay.developer,
                       {{"team", relay.fixture.host->teamMetadata().value(relay.developer)}}}});
        QCOMPARE(sealed.value("team").toObject().value("role"), QJsonValue("developer"));
        QCOMPARE(sealed.value("team").toObject().value("id"), QJsonValue(relay.team));
    }
};

QTEST_GUILESS_MAIN(TeamRemoteCloudTest)
#include "TeamRemoteCloudTest.moc"
