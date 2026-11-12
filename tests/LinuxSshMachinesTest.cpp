#include "remote/LinuxSshMachines.h"
#include "FakeRelayLink.h"
#include "daemon/FakeSshLink.h"
#include "remote/LinuxMachineModel.h"
#include "remote/LinuxMachinesSection.h"
#include "remote/LinuxRelayMachines.h"
#include "ssh/KnownHostsFile.h"
#include "ssh/SshMachineStore.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus;
using remote::ssh::HostKey;
using remote::ssh::SshMachineStore;
using remote::ssh::SshTunnel;
using remote::ssh::testing::FakeDaemon;
using remote::ssh::testing::FakeLink;

namespace {

constexpr auto host = "dev@devbox:2222";
const HostKey devboxKey{QStringLiteral("[devbox]:2222"), QStringLiteral("ssh-ed25519"),
                        QStringLiteral("AAAAC3NzaC1lZDI1NTE5AAAAIA")};

// A temporary store and known_hosts file, and links to a fake daemon that the test can reach.
struct Fixture {
    Fixture()
        : name(QStringLiteral("ketplus-linux-ssh-%1").arg(QCoreApplication::applicationPid())),
          daemon(name) {}

    [[nodiscard]] QString storePath() const { return directory.filePath("machines.json"); }
    [[nodiscard]] QString knownHosts() const { return directory.filePath("known_hosts"); }

    LinuxSshMachines::LinkFactory links() {
        return [this](const remote::ssh::SshTarget&) -> std::unique_ptr<remote::ssh::SshLink> {
            auto link = std::make_unique<FakeLink>(name);
            link->knownHosts = knownHosts();
            link->autoConnect = autoConnect;
            opened.append(link.get());
            return link;
        };
    }

    [[nodiscard]] const LinuxMachine* only(const LinuxMachineModel& model) const {
        return model.machines().size() == 1 ? &model.machines().first() : nullptr;
    }

    QTemporaryDir directory;
    QString name;
    FakeDaemon daemon;
    bool autoConnect{true};
    QList<QPointer<FakeLink>> opened;
};

} // namespace

class LinuxSshMachinesTest final : public QObject {
    Q_OBJECT

  private slots:
    void connectsFromTheCardAndKeysByTheDaemonIdentity() {
        Fixture fixture;
        LinuxMachineModel model;
        LinuxSshMachines machines(&model, fixture.storePath(), fixture.links());
        QVERIFY(model.backendsAttached());
        QSignalSpy connected(&model, &LinuxMachineModel::sshConnected);
        model.requestSshConnect(QString::fromLatin1(host));
        QTRY_COMPARE(connected.size(), 1);
        QCOMPARE(connected.first().first().toString(), QString::fromLatin1(host));
        const LinuxMachine* machine = fixture.only(model);
        QVERIFY(machine);
        // ADR 0119: the daemon's host key fingerprint names the machine, not the SSH target.
        QCOMPARE(machine->id, fixture.daemon.fingerprint);
        QCOMPARE(machine->name, QStringLiteral("devbox"));
        QCOMPARE(machine->route, LinuxMachine::Route::Ssh);
        QCOMPARE(machine->state, LinuxMachine::State::Connected);
        QCOMPARE(machine->fingerprint, QStringLiteral("SHA256:sshkey"));
        QCOMPARE(machine->sshTarget, QString::fromLatin1(host));
        QVERIFY(machines.backend(machine->id));
        // Persisted with its identity.
        const SshMachineStore saved(fixture.storePath());
        QCOMPARE(saved.machineId(QString::fromLatin1(host)), fixture.daemon.fingerprint);
    }

    void stateFollowsTheBackendLive() {
        Fixture fixture;
        LinuxMachineModel model;
        LinuxSshMachines machines(&model, fixture.storePath(), fixture.links());
        model.requestSshConnect(QString::fromLatin1(host));
        QTRY_VERIFY(fixture.only(model) &&
                    fixture.only(model)->state == LinuxMachine::State::Connected);
        const QString id = fixture.only(model)->id;
        // A dropped session reconnects on its own.
        fixture.opened.last()->autoConnect = false;
        fixture.opened.last()->dropConnection();
        QTRY_COMPARE(model.machine(id)->state, LinuxMachine::State::Reconnecting);
        // Disconnect and Connect act on the backend.
        model.requestDisconnect(id);
        QTRY_COMPARE(model.machine(id)->state, LinuxMachine::State::Offline);
        QCOMPARE(model.machine(id)->error, LinuxMachine::Error::None);
        fixture.opened.last()->autoConnect = true;
        model.requestConnect(id);
        QTRY_COMPARE(model.machine(id)->state, LinuxMachine::State::Connected);
        // A refused SSH key is an error the user can act on.
        fixture.opened.last()->setTunnelState(SshTunnel::State::AuthFailed);
        QTRY_COMPARE(model.machine(id)->error, LinuxMachine::Error::AuthenticationFailed);
        QCOMPARE(model.machine(id)->state, LinuxMachine::State::Offline);
    }

    void savedMachinesReturnAndConnectOnActivate() {
        Fixture fixture;
        QString id;
        {
            LinuxMachineModel model;
            LinuxSshMachines machines(&model, fixture.storePath(), fixture.links());
            model.requestSshConnect(QString::fromLatin1(host));
            QTRY_VERIFY(fixture.only(model) &&
                        fixture.only(model)->state == LinuxMachine::State::Connected);
            id = fixture.only(model)->id;
        }
        LinuxMachineModel model;
        LinuxSshMachines machines(&model, fixture.storePath(), fixture.links());
        QVERIFY(model.machine(id));
        machines.connectSaved();
        QTRY_COMPARE(model.machine(id)->state, LinuxMachine::State::Connected);
    }

    void oneHostOnBothRoutesIsOneSshMachine() {
        Fixture fixture;
        LinuxMachineModel model;
        LinuxSshMachines machines(&model, fixture.storePath(), fixture.links());
        model.setRelayMachines({{QStringLiteral("relay-7"), fixture.daemon.fingerprint},
                                {QStringLiteral("relay-8"), QStringLiteral("other-host")}});
        QCOMPARE(model.machines().size(), 2);
        model.requestSshConnect(QString::fromLatin1(host));
        QTRY_VERIFY(model.machine(fixture.daemon.fingerprint) &&
                    model.machine(fixture.daemon.fingerprint)->route == LinuxMachine::Route::Ssh);
        QCOMPARE(model.machines().size(), 2);
        const auto* both = model.machine(fixture.daemon.fingerprint);
        // SSH preferred; the Relay's own id is only a route attribute.
        QCOMPARE(both->relayId, QStringLiteral("relay-7"));
        QCOMPARE(model.machines().first().id, fixture.daemon.fingerprint);
        // Without SSH the Relay row is back.
        model.requestRemove(both->id);
        QTRY_COMPARE(model.machine(fixture.daemon.fingerprint)->route, LinuxMachine::Route::Relay);
    }

    void aFirstUseKeyIsAcceptedOnlyFromTheCard() {
        Fixture fixture;
        fixture.autoConnect = false;
        LinuxMachineModel model;
        LinuxSshMachines machines(&model, fixture.storePath(), fixture.links());
        QSignalSpy presented(&model, &LinuxMachineModel::hostKeyPresented);
        QSignalSpy connected(&model, &LinuxMachineModel::sshConnected);
        model.requestSshConnect(QString::fromLatin1(host));
        // Not listed while the card is still adding it.
        QVERIFY(model.machines().isEmpty());
        FakeLink* link = fixture.opened.last();
        link->offerKey(devboxKey);
        QCOMPARE(presented.size(), 1);
        QCOMPARE(presented.first().at(1).toString(), devboxKey.fingerprint());
        // A decision for another key than the one shown never trusts it.
        model.decideHostKey(QString::fromLatin1(host), QStringLiteral("SHA256:other"), true);
        QVERIFY(!remote::ssh::hasKnownHost(fixture.knownHosts(), devboxKey.knownHostsName));
        link->offerKey(devboxKey);
        model.decideHostKey(QString::fromLatin1(host), devboxKey.fingerprint(), true);
        QVERIFY(remote::ssh::hasKnownHost(fixture.knownHosts(), devboxKey.knownHostsName));
        link->autoConnect = true;
        link->open();
        QTRY_COMPARE(connected.size(), 1);
        QCOMPARE(SshMachineStore(fixture.storePath()).acceptedHostKeys(QString::fromLatin1(host)),
                 QStringList{devboxKey.knownHostsName});
    }

    void aRejectedOrCancelledNewMachineIsNotKept() {
        Fixture fixture;
        fixture.autoConnect = false;
        LinuxMachineModel model;
        LinuxSshMachines machines(&model, fixture.storePath(), fixture.links());
        QSignalSpy failed(&model, &LinuxMachineModel::sshFailed);
        model.requestSshConnect(QString::fromLatin1(host));
        QVERIFY(SshMachineStore(fixture.storePath()).contains(QString::fromLatin1(host)));
        fixture.opened.last()->offerKey(devboxKey);
        model.decideHostKey(QString::fromLatin1(host), devboxKey.fingerprint(), false);
        fixture.opened.last()->setTunnelState(SshTunnel::State::HostKeyRejected);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(!SshMachineStore(fixture.storePath()).contains(QString::fromLatin1(host)));
        QVERIFY(model.machines().isEmpty());

        model.requestSshConnect(QString::fromLatin1(host));
        model.cancelSshConnect(QString::fromLatin1(host));
        QTRY_VERIFY(!SshMachineStore(fixture.storePath()).contains(QString::fromLatin1(host)));
        QVERIFY(model.machines().isEmpty());
    }

    void aMismatchIsNeverVerifiedAndCanBeForgotten() {
        Fixture fixture;
        fixture.autoConnect = false;
        LinuxMachineModel model;
        LinuxSshMachines machines(&model, fixture.storePath(), fixture.links());
        model.requestSshConnect(QString::fromLatin1(host));
        FakeLink* link = fixture.opened.last();
        link->offerKey(devboxKey);
        model.decideHostKey(QString::fromLatin1(host), devboxKey.fingerprint(), true);
        link->autoConnect = true;
        link->open();
        QTRY_VERIFY(fixture.only(model) &&
                    fixture.only(model)->state == LinuxMachine::State::Connected);
        const QString id = fixture.only(model)->id;

        // The host now offers another key: shown as offered, never as the machine's key.
        link->setTunnelState(SshTunnel::State::HostKeyMismatch);
        QTRY_COMPARE(model.machine(id)->error, LinuxMachine::Error::HostKeyMismatch);
        QVERIFY(model.machine(id)->fingerprint.isEmpty());
        QCOMPARE(model.machine(id)->presentedFingerprint, QStringLiteral("SHA256:sshkey"));
        QVERIFY(model.machine(id)->canForgetHostKey);
        QVERIFY(LinuxMachineModel::errorMessage(*model.machine(id)).contains("Forget old key"));

        // Forgetting removes the key ketPlus accepted and asks the card to connect again.
        QSignalSpy prompted(&model, &LinuxMachineModel::sshConnectPrompted);
        model.requestForgetHostKey(id);
        QCOMPARE(prompted.size(), 1);
        QCOMPARE(prompted.first().first().toString(), QString::fromLatin1(host));
        QVERIFY(!remote::ssh::hasKnownHost(fixture.knownHosts(), devboxKey.knownHostsName));
        QVERIFY(!model.machine(id)->canForgetHostKey);
        // A second time the key is the user's own: ketPlus explains instead.
        QSignalSpy notices(&machines, &LinuxSshMachines::notice);
        model.requestForgetHostKey(id);
        QCOMPARE(notices.size(), 1);
        // ssh files a key for port 2222 as "[devbox]:2222", quoted so the shell keeps it whole.
        QVERIFY(notices.first().first().toString().contains("ssh-keygen -R '[devbox]:2222'"));
    }

    void removingDeletesTheIdentityAndTheAcceptedKey() {
        Fixture fixture;
        fixture.autoConnect = false;
        QVERIFY(remote::ssh::recordHostKey(
            fixture.knownHosts(), HostKey{QStringLiteral("other"), QStringLiteral("ssh-ed25519"),
                                          QStringLiteral("AAAAC3NzaC1lZDI1NTE5AAAAIB")}));
        LinuxMachineModel model;
        LinuxSshMachines machines(&model, fixture.storePath(), fixture.links());
        model.requestSshConnect(QString::fromLatin1(host));
        FakeLink* link = fixture.opened.last();
        link->offerKey(devboxKey);
        model.decideHostKey(QString::fromLatin1(host), devboxKey.fingerprint(), true);
        link->autoConnect = true;
        link->open();
        QTRY_VERIFY(fixture.only(model) &&
                    fixture.only(model)->state == LinuxMachine::State::Connected);
        model.requestRemove(fixture.only(model)->id);
        QTRY_VERIFY(model.machines().isEmpty());
        const SshMachineStore saved(fixture.storePath());
        QVERIFY(saved.machines().isEmpty());
        QVERIFY(!remote::ssh::hasKnownHost(fixture.knownHosts(), devboxKey.knownHostsName));
        QVERIFY(remote::ssh::hasKnownHost(fixture.knownHosts(), QStringLiteral("other")));
    }

    void knownHostsNamesAreTheOnesSshFilesKeysUnder() {
        QCOMPARE(LinuxMachineModel::knownHostsName(QStringLiteral("build-node-02")),
                 QStringLiteral("build-node-02"));
        QCOMPARE(LinuxMachineModel::knownHostsName(QStringLiteral("dev@build-node-02:22")),
                 QStringLiteral("build-node-02"));
        QCOMPARE(LinuxMachineModel::knownHostsName(QStringLiteral("dev@build-node-02:2222")),
                 QStringLiteral("[build-node-02]:2222"));
        QCOMPARE(LinuxMachineModel::knownHostsName(QStringLiteral("[2001:db8::1]:2222")),
                 QStringLiteral("[2001:db8::1]:2222"));
        QCOMPARE(LinuxMachineModel::knownHostsName(QStringLiteral("[2001:db8::1]")),
                 QStringLiteral("2001:db8::1"));
        // A bare IPv6 address has no port to split off.
        QCOMPARE(LinuxMachineModel::knownHostsName(QStringLiteral("2001:db8::1")),
                 QStringLiteral("2001:db8::1"));
        QCOMPARE(LinuxMachineModel::knownHostsName(QStringLiteral("dev@2001:db8::1")),
                 QStringLiteral("2001:db8::1"));
        QCOMPARE(LinuxMachineModel::knownHostsName(QStringLiteral("dev@[2001:db8::1]:22")),
                 QStringLiteral("2001:db8::1"));
        // A saved target is named from its parts: its key "2001:db8::1:2222" would be ambiguous.
        remote::ssh::SshTarget v6;
        v6.alias = QStringLiteral("2001:db8::1");
        v6.port = 2222;
        QCOMPARE(remote::ssh::sshKnownHostsName(v6), QStringLiteral("[2001:db8::1]:2222"));
        QCOMPARE(LinuxMachineModel::knownHostsCommandWord(remote::ssh::sshKnownHostsName(v6)),
                 QStringLiteral("'[2001:db8::1]:2222'"));
        LinuxMachine machine;
        machine.name = QStringLiteral("build-node-02");
        machine.sshTarget = QStringLiteral("dev@build-node-02:2222");
        machine.knownHostsName = QStringLiteral("[build-node-02]:2222");
        machine.error = LinuxMachine::Error::HostKeyMismatch;
        QVERIFY(LinuxMachineModel::errorMessage(machine).endsWith(
            QStringLiteral("ssh-keygen -R '[build-node-02]:2222'")));
    }

    void connectAndDisconnectActOnTheRouteTheRowShows() {
        Fixture fixture;
        QString id;
        {
            LinuxMachineModel model;
            LinuxSshMachines machines(&model, fixture.storePath(), fixture.links());
            model.requestSshConnect(QString::fromLatin1(host));
            QTRY_VERIFY(fixture.only(model) &&
                        fixture.only(model)->state == LinuxMachine::State::Connected);
            id = fixture.only(model)->id;
        }
        LinuxMachineModel model;
        LinuxSshMachines machines(&model, fixture.storePath(), fixture.links());
        QPointer<ketplus::testing::FakeRelayLink> relayLink;
        LinuxRelayMachines relay(&model, [&relayLink](const QString& relayId) {
            auto made = std::make_unique<ketplus::testing::FakeRelayLink>(relayId);
            relayLink = made.get();
            return made;
        });
        relay.setEnabled(true);
        relay.setListed({{QStringLiteral("relay-1"), id, QStringLiteral("devbox"), {}}});
        QTRY_VERIFY(relayLink && relayLink->connects == 1);
        // SSH cannot reach the host at launch: the Relay stands in and says why.
        fixture.autoConnect = false;
        machines.connectSaved();
        QVERIFY(!fixture.opened.isEmpty());
        auto* sshLink = fixture.opened.last().data();
        sshLink->setTunnelState(SshTunnel::State::Offline);
        QTRY_COMPARE(model.machine(id)->route, LinuxMachine::Route::Relay);
        QVERIFY(!model.machine(id)->sshFailure.isEmpty());

        // Disconnect and Connect are the Relay's; the SSH link is left as it is.
        model.requestDisconnect(id);
        QCOMPARE(relayLink->disconnects, 1);
        QCOMPARE(sshLink->state(), SshTunnel::State::Offline);
        QCOMPARE(model.machine(id)->route, LinuxMachine::Route::Relay);
        const int opens = sshLink->opens;
        model.requestConnect(id);
        QCOMPARE(relayLink->connects, 2);
        QCOMPARE(sshLink->opens, opens);
        QCOMPARE(model.machine(id)->state, LinuxMachine::State::Connected);

        // Use SSH tries SSH again; once it answers, the row is SSH's.
        sshLink->autoConnect = true;
        fixture.autoConnect = true;
        model.requestSshRoute(id);
        QTRY_VERIFY(model.machine(id)->route == LinuxMachine::Route::Ssh &&
                    model.machine(id)->state == LinuxMachine::State::Connected);
        QVERIFY(model.machine(id)->sshFailure.isEmpty());

        // A user Disconnect is not "SSH down": the row stays SSH's, and the Relay is untouched.
        model.requestDisconnect(id);
        QTRY_COMPARE(model.machine(id)->state, LinuxMachine::State::Offline);
        QCOMPARE(model.machine(id)->route, LinuxMachine::Route::Ssh);
        QCOMPARE(model.machine(id)->error, LinuxMachine::Error::None);
        QCOMPARE(relayLink->disconnects, 1);
        // Opening Connections again does not undo it; Connect does.
        machines.connectSaved();
        QTest::qWait(50);
        QCOMPARE(model.machine(id)->state, LinuxMachine::State::Offline);
        model.requestConnect(id);
        QTRY_COMPARE(model.machine(id)->state, LinuxMachine::State::Connected);
        QCOMPARE(model.machine(id)->route, LinuxMachine::Route::Ssh);
    }

    void thePickerAndTheSelectionSeamFollowTheMachines() {
        Fixture fixture;
        LinuxMachinesSection section;
        section.attachSshBackend(fixture.storePath(), fixture.links());
        QSignalSpy picker(&section, &LinuxMachinesSection::machinesChanged);
        section.model()->requestSshConnect(QString::fromLatin1(host));
        QTRY_VERIFY(!picker.isEmpty() &&
                    picker.last().at(1).toStringList() ==
                        QStringList{QStringLiteral("devbox · SSH · Connected")});
        // The window's MachineSelection registers each picker id's backend from the section.
        const QString id = picker.last().at(0).toStringList().first();
        QVERIFY(section.machineBackend(id));
        QCOMPARE(section.machineBackend(id)->machineId(), id);
        QVERIFY(!section.machineBackend(QStringLiteral("unknown")));
    }
};

QTEST_MAIN(LinuxSshMachinesTest)
#include "LinuxSshMachinesTest.moc"
