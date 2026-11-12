#include "remote/LinuxRelayMachines.h"
#include "FakeRelayLink.h"
#include "remote/LinuxMachineModel.h"
#include "remote/LinuxMachineRow.h"

#include "KetDialog.h"

#include <QAbstractButton>
#include <QDateTime>
#include <QFrame>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>

// Connections' Relay route (ADR 0119) against fake Relay links: live state, the entitlement
// gate, the fingerprint keying, host presence and pending outcomes.
namespace ketplus {

using Error = LinuxMachine::Error;
using Route = LinuxMachine::Route;
using State = LinuxMachine::State;
using ConnectionState = remote::MachineBackend::ConnectionState;
using testing::FakeRelayLink;

namespace {

const QString pinned = QStringLiteral("SHA256:pinnedhostkey");

} // namespace

class LinuxRelayMachinesTest : public QObject {
    Q_OBJECT

  private slots:
    void init() { links_.clear(); }

    void theRelayRunsOnlyWhileEntitled() {
        LinuxMachineModel model;
        LinuxRelayMachines relay(&model, factory());
        relay.setListed({{QStringLiteral("relay-1"), pinned, QStringLiteral("builder"), {}}});
        QCOMPARE(model.machines().size(), 1);
        auto machine = model.machines().first();
        QCOMPARE(machine.id, pinned);
        QCOMPARE(machine.name, QStringLiteral("builder"));
        QCOMPARE(machine.relayId, QStringLiteral("relay-1"));
        QCOMPARE(machine.state, State::Offline);
        QVERIFY(!machine.connectable);
        QCOMPARE(links_.value(QStringLiteral("relay-1"))->connects, 0);
        // Not entitled: Connect does nothing.
        model.requestConnect(pinned);
        QCOMPARE(links_.value(QStringLiteral("relay-1"))->connects, 0);

        relay.setEnabled(true);
        QCOMPARE(links_.value(QStringLiteral("relay-1"))->connects, 1);
        machine = model.machines().first();
        QCOMPARE(machine.state, State::Connected);
        QVERIFY(machine.connectable);
        QCOMPARE(machine.error, Error::None);
        QCOMPARE(relay.backend(pinned), links_.value(QStringLiteral("relay-1"))->backend());

        relay.setEnabled(false);
        QCOMPARE(links_.value(QStringLiteral("relay-1"))->disconnects, 1);
        machine = model.machines().first();
        QCOMPARE(machine.state, State::Offline);
        QVERIFY(!machine.connectable);
        QCOMPARE(machine.error, Error::None);
    }

    void liveStateFollowsTheBackend() {
        LinuxMachineModel model;
        LinuxRelayMachines relay(&model, factory());
        relay.setEnabled(true);
        relay.setListed({{QStringLiteral("relay-1"), pinned, {}, {}}});
        FakeRelayLink* link = links_.value(QStringLiteral("relay-1"));
        QVERIFY(link);
        QCOMPARE(model.machines().first().state, State::Connected);
        QCOMPARE(model.machines().first().name, QStringLiteral("relay-1"));

        link->backend_->setState(ConnectionState::Reconnecting,
                                 QStringLiteral("Relay unreachable."));
        QCOMPARE(model.machines().first().state, State::Reconnecting);
        QCOMPARE(model.machines().first().error, Error::None);
        // Offline on its own is a failure the user sees, with the backend's reason.
        link->backend_->setState(ConnectionState::Offline, QStringLiteral("Account changed."));
        QCOMPARE(model.machines().first().state, State::Offline);
        QCOMPARE(model.machines().first().error, Error::Other);
        QCOMPARE(model.machines().first().errorText, QStringLiteral("Account changed."));

        model.requestConnect(pinned);
        QCOMPARE(link->connects, 2);
        QCOMPARE(model.machines().first().state, State::Connected);
        QCOMPARE(model.machines().first().error, Error::None);
        // A disconnect the user asked for is not a failure.
        model.requestDisconnect(pinned);
        QCOMPARE(link->disconnects, 1);
        QCOMPARE(model.machines().first().state, State::Offline);
        QCOMPARE(model.machines().first().error, Error::None);
        // And it stays disconnected when the entitlement is confirmed again.
        relay.setEnabled(false);
        relay.setEnabled(true);
        QCOMPARE(link->connects, 2);
    }

    void aHostOnBothRoutesIsOneMachineWithSshPreferred() {
        LinuxMachineModel model;
        LinuxMachine ssh;
        ssh.id = pinned;
        ssh.name = QStringLiteral("build-node-02");
        ssh.route = Route::Ssh;
        ssh.state = State::Connected;
        ssh.fingerprint = pinned;
        ssh.sshTarget = QStringLiteral("build-node-02");
        model.setSshMachines({ssh});
        LinuxRelayMachines relay(&model, factory());
        relay.setEnabled(true);
        relay.setListed({{QStringLiteral("relay-1"), pinned, QStringLiteral("builder"), {}}});
        QCOMPARE(model.machines().size(), 1);
        QCOMPARE(model.machines().first().route, Route::Ssh);
        QCOMPARE(model.machines().first().relayId, QStringLiteral("relay-1"));
        // Its Connect and Disconnect are the SSH backend's.
        model.requestDisconnect(pinned);
        QCOMPARE(links_.value(QStringLiteral("relay-1"))->disconnects, 0);

        // A host paired before host pins is its own machine until it has a fingerprint.
        relay.setListed(
            {{QStringLiteral("relay-1"), pinned, {}, {}}, {QStringLiteral("relay-2"), {}, {}, {}}});
        QCOMPARE(model.machines().size(), 2);
        const auto* unpinned = model.machine(QStringLiteral("relay:relay-2"));
        QVERIFY(unpinned);
        QCOMPARE(unpinned->route, Route::Relay);
        QVERIFY(unpinned->fingerprint.isEmpty());
        QVERIFY(!unpinned->detail.contains(QStringLiteral("Trusted")));
        QCOMPARE(relay.backend(QStringLiteral("relay:relay-2")),
                 links_.value(QStringLiteral("relay-2"))->backend());

        // An unlisted host loses its link.
        QPointer<FakeRelayLink> gone = links_.value(QStringLiteral("relay-2"));
        relay.setListed({{QStringLiteral("relay-1"), pinned, {}, {}}});
        QVERIFY(gone.isNull());
        QVERIFY(!model.machine(QStringLiteral("relay:relay-2")));
    }

    void theRelayIsTheFallbackWhileSshCannotReachTheHost() {
        LinuxMachineModel model;
        LinuxMachine ssh;
        ssh.id = pinned;
        ssh.name = QStringLiteral("build-node-02");
        ssh.route = Route::Ssh;
        ssh.state = State::Offline;
        ssh.error = Error::Other;
        ssh.errorText = QStringLiteral("Connection timed out.");
        ssh.sshTarget = QStringLiteral("build-node-02");
        model.setSshMachines({ssh});
        LinuxRelayMachines relay(&model, factory());
        relay.setEnabled(true);
        relay.setListed({{QStringLiteral("relay-1"), pinned, QStringLiteral("builder"), {}}});
        // SSH is down and the Relay answers: the one machine is reached over the Relay.
        QCOMPARE(model.machines().size(), 1);
        auto machine = model.machines().first();
        QCOMPARE(machine.route, Route::Relay);
        QCOMPARE(machine.state, State::Connected);
        QCOMPARE(machine.name, QStringLiteral("build-node-02"));
        QCOMPARE(machine.sshTarget, QStringLiteral("build-node-02"));
        QCOMPARE(machine.error, Error::None);
        // Its Disconnect is the Relay's.
        model.requestDisconnect(pinned);
        QCOMPARE(links_.value(QStringLiteral("relay-1"))->disconnects, 1);
        QCOMPARE(model.machines().first().route, Route::Ssh);
        model.requestConnect(pinned); // SSH's to carry out: the Relay link stays stopped
        QCOMPARE(links_.value(QStringLiteral("relay-1"))->connects, 1);

        // A key mismatch is never hidden behind the Relay.
        relay.setEnabled(false);
        relay.setEnabled(true);
        ssh.error = Error::HostKeyMismatch;
        model.setSshMachines({ssh});
        links_.value(QStringLiteral("relay-1"))->connectToRelay();
        QCOMPARE(model.machines().first().route, Route::Ssh);
        QCOMPARE(model.machines().first().error, Error::HostKeyMismatch);
        // Nor is the Relay used without ketPlus Pro.
        ssh.error = Error::Other;
        model.setSshMachines({ssh});
        relay.setEnabled(false);
        QCOMPARE(model.machines().first().route, Route::Ssh);
    }

    void hostPresenceIsShownApartFromTheRelayLink() {
        const auto now = QDateTime::currentDateTimeUtc();
        LinuxMachineModel model;
        model.setBackendsAttached(true);
        LinuxRelayMachines relay(&model, factory());
        relay.setEnabled(true);
        relay.setListed({{QStringLiteral("relay-1"), pinned, QStringLiteral("builder"), now}});
        auto machine = model.machines().first();
        QCOMPARE(machine.state, State::Connected);
        QVERIFY(!LinuxMachineModel::hostAway(machine, now));
        QCOMPARE(LinuxMachineModel::presenceText(machine, now), QStringLiteral("Host online"));
        QCOMPARE(LinuxMachineModel::pickerLabel(machine),
                 QStringLiteral("builder · Relay · Connected"));

        relay.setListed({{QStringLiteral("relay-1"), pinned, QStringLiteral("builder"),
                          now.addSecs(-10 * 60)}});
        machine = model.machines().first();
        QCOMPARE(machine.state, State::Connected);
        QVERIFY(LinuxMachineModel::hostAway(machine, now));
        QCOMPARE(LinuxMachineModel::presenceText(machine, now),
                 QStringLiteral("Host offline, last seen 10 min ago"));
        QCOMPARE(LinuxMachineModel::pickerLabel(machine),
                 QStringLiteral("builder · Relay · Host offline"));
        QCOMPARE(linuxMachineBadgeText(machine, true), QStringLiteral("Host offline"));
        machine.hostLastSeen = now.addSecs(-3 * 3600);
        QCOMPARE(LinuxMachineModel::presenceText(machine, now),
                 QStringLiteral("Host offline, last seen 3 h ago"));

        // The row says it, and Connect does not promise the host runs anything now.
        QWidget window;
        window.resize(800, 600);
        window.show();
        auto* row = makeLinuxMachineRow(model.machines().first(), &model, &window);
        auto* presence = row->findChild<QLabel*>(QStringLiteral("linuxMachinePresence"));
        QVERIFY(presence);
        QVERIFY2(presence->text().startsWith(QStringLiteral("Host offline, last seen 10 min ago")),
                 qPrintable(presence->text()));
        // SSH machines show no presence: Connected over SSH means the host answers.
        LinuxMachine ssh = model.machines().first();
        ssh.route = Route::Ssh;
        QVERIFY(LinuxMachineModel::presenceText(ssh, now).isEmpty());
        QVERIFY(!LinuxMachineModel::hostAway(ssh, now));

        links_.value(QStringLiteral("relay-1"))->disconnectFromRelay();
        delete row;
        row = makeLinuxMachineRow(model.machines().first(), &model, &window);
        auto* toggle = row->findChild<QAbstractButton*>(QStringLiteral("linuxMachineToggle"));
        QVERIFY(toggle);
        QCOMPARE(toggle->text(), QStringLiteral("Connect"));
        QVERIFY(toggle->toolTip().contains(QStringLiteral("wait until the host is back")));

        showLinuxMachineDetails(pinned, &model, row);
        auto* dialog = window.findChild<ket::Dialog*>(QStringLiteral("linuxMachineDetailsDialog"));
        QVERIFY(dialog);
        auto* host = dialog->findChild<QLabel*>(QStringLiteral("linuxDetailsHost"));
        QVERIFY(host);
        QCOMPARE(host->text(), QStringLiteral("Host offline, last seen 10 min ago"));
        auto* wait = dialog->findChild<QLabel*>(QStringLiteral("linuxDetailsWait"));
        QVERIFY(wait && wait->text().contains(QStringLiteral("up to 15 minutes")));
        // The pinned host identity is shown as verified, and a Relay pairing is not removed here.
        auto* key = dialog->findChild<QLabel*>(QStringLiteral("linuxDetailsFingerprint"));
        QVERIFY(key);
        QCOMPARE(key->text(), pinned);
        QCOMPARE(dialog->findChild<QLabel*>(QStringLiteral("linuxDetailsVerified"))->text(),
                 QStringLiteral("Verified"));
        QVERIFY(!dialog->findChild<QAbstractButton*>(QStringLiteral("linuxDetailsRemove")));
        dialog->reject();
        delete row;
    }

    void pendingOutcomesAreShownUntilTheyComplete() {
        LinuxMachineModel model;
        LinuxRelayMachines relay(&model, factory());
        relay.setEnabled(true);
        relay.setListed({{QStringLiteral("relay-1"), pinned, QStringLiteral("builder"),
                          QDateTime::currentDateTimeUtc()}});
        FakeRelayLink* link = links_.value(QStringLiteral("relay-1"));
        link->setPending(2);
        QCOMPARE(model.machines().first().pendingCommands, 2);
        QWidget window;
        auto* row = makeLinuxMachineRow(model.machines().first(), &model, &window);
        auto* note = row->findChild<QLabel*>(QStringLiteral("linuxMachinePresence"));
        QVERIFY(note);
        QCOMPARE(note->text(), QStringLiteral("2 requests sent, outcome pending"));
        delete row;
        link->setPending(0);
        QCOMPARE(model.machines().first().pendingCommands, 0);
        row = makeLinuxMachineRow(model.machines().first(), &model, &window);
        QVERIFY(!row->findChild<QLabel*>(QStringLiteral("linuxMachinePresence")));
        delete row;
    }

    void withoutProTheRouteOffersAnUpgradeAndNoConnect() {
        LinuxMachineModel model;
        model.setBackendsAttached(true);
        LinuxRelayMachines relay(&model, factory());
        relay.setListed({{QStringLiteral("relay-1"), pinned, QStringLiteral("builder"), {}}});
        const auto machine = model.machines().first();
        QCOMPARE(linuxMachineBadgeText(machine, true), QStringLiteral("Needs Pro"));
        QWidget window;
        auto* row = makeLinuxMachineRow(machine, &model, &window);
        QVERIFY(!row->findChild<QAbstractButton*>(QStringLiteral("linuxMachineToggle")));
        auto* note = row->findChild<QLabel*>(QStringLiteral("linuxMachinePresence"));
        QVERIFY(note);
        QVERIFY(note->text().contains(QStringLiteral("ketPlus Pro")));
        QVERIFY(note->text().contains(QStringLiteral("KetSuite account")));
        QVERIFY(note->text().contains(QStringLiteral("SSH stays free")));
        delete row;
    }

    void eachHostHasItsOwnOutboxFile() {
        const auto one = LinuxRelayMachines::outboxFile(QStringLiteral("relay-1"));
        const auto two = LinuxRelayMachines::outboxFile(QStringLiteral("relay-2"));
        QVERIFY(one != two);
        QVERIFY(one.endsWith(QStringLiteral(".json")));
        QVERIFY(one.contains(QStringLiteral("/relay/outbox-")));
        QVERIFY(
            !LinuxRelayMachines::outboxFile(QStringLiteral("../x")).contains(QStringLiteral("..")));
    }

  private:
    LinuxRelayMachines::LinkFactory factory() {
        return [this](const QString& relayId) {
            auto link = std::make_unique<FakeRelayLink>(relayId);
            links_.insert(relayId, link.get());
            return link;
        };
    }

    QHash<QString, QPointer<FakeRelayLink>> links_;
};

} // namespace ketplus

QTEST_MAIN(ketplus::LinuxRelayMachinesTest)
#include "LinuxRelayMachinesTest.moc"
