#include "remote/LinuxMachinesSection.h"
#include "FakeRelayLink.h"
#include "control/ControlV2Client.h"
#include "daemon/FakeSshLink.h"
#include "remote/AccountEntitlements.h"
#include "remote/LinuxMachineModel.h"
#include "remote/LinuxMachineRow.h"

#include "KetButton.h"
#include "KetDialog.h"
#include "KetSegmentedControl.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QFrame>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace ketplus::remote {
class RemoteControlTestAccess {
  public:
    static void signIn(ControlV2Client& client) {
        client.credential_.insert(QStringLiteral("deviceId"), QStringLiteral("test-desktop"));
    }
    static void pin(ControlV2Client& client, const QJsonObject& pins) { client.hostPins_ = pins; }
    static void signOut(ControlV2Client& client) { client.credential_ = {}; }
    static void entitle(ControlV2Client& client, const QJsonArray& entitlements) {
        client.credential_.insert(QStringLiteral("entitlements"), entitlements);
    }
};
} // namespace ketplus::remote

namespace ketplus {

using Error = LinuxMachine::Error;
using Route = LinuxMachine::Route;
using State = LinuxMachine::State;

class LinuxMachinesSectionTest final : public QObject {
    Q_OBJECT

  private slots:
    void validatesSshHosts_data();
    void validatesSshHosts();
    void rowsShowStateRouteAndErrors();
    void rowButtonsAndDetailsEmitIntents();
    void sshModeAcceptsAndRejectsAFirstUseHostKey();
    void sshFailuresAreActionable();
    void relayModeOpensPairingAndRespectsTheEntitlement();
    void relayRefreshKeepsSshMachines();
    void pinnedFingerprintNamesTheRelayHost();
    void withoutBackendsOnlyTheRelayIsOffered();
    void signedOutOffersSshWithoutAnAccount();
    void detailsFollowTheModel();
    void enterConnectsAndAHostKeyFocusesReject();
    void detailsShowAnOfferedKeyAsAMismatchAndOfferRetrust();
    void anIdentityChangeShowsBothIdentitiesBeforeTrust();
    void theRelayFollowsTheAccountEntitlement();
    void theRelayStopsWhenTheSessionIsRevoked();
    void aForgottenKeyReopensTheCardOnThatHost();
    void savedMachinesConnectAtLaunchBeforeTheCardIsShown();
    void aRelayFallbackRowSaysWhySshFailedAndOffersSsh();

  private:
    static LinuxMachine make(const QString& id, const QString& name, const QString& detail,
                             Route route, State state, const QString& fingerprint = {},
                             Error error = Error::None);
    static QList<LinuxMachine> fleet();
    static QFrame* row(QWidget& section, const QString& id);
    static ket::Dialog* connectDialog(QWidget& section);
    static QAbstractButton* button(QWidget* parent, const QString& objectName);
    static QString text(QWidget* parent, const QString& objectName);
};

LinuxMachine LinuxMachinesSectionTest::make(const QString& id, const QString& name,
                                            const QString& detail, const Route route,
                                            const State state, const QString& fingerprint,
                                            const Error error) {
    LinuxMachine machine;
    machine.id = id;
    machine.name = name;
    machine.detail = detail;
    machine.route = route;
    machine.state = state;
    machine.fingerprint = fingerprint;
    machine.error = error;
    return machine;
}

QList<LinuxMachine> LinuxMachinesSectionTest::fleet() {
    return {
        make("ssh-1", "build-node-01", "Ubuntu 24.04 LTS · Ready for remote work", Route::Ssh,
             State::Connected, "SHA256:9f2cQx0aA1bB2cC3dD4eE5fF6gG7hH8iI9jJ0kKa41e"),
        make("relay-1", "build-node-02", "Linux · Trusted", Route::Relay, State::Offline),
        make("ssh-2", "gpu-box", "Debian 13", Route::Ssh, State::Connecting),
        make("ssh-3", "ci-runner", "Fedora 41", Route::Ssh, State::Reconnecting),
        make("ssh-4", "old-host", "Ubuntu 22.04", Route::Ssh, State::Offline, {},
             Error::HostKeyMismatch),
    };
}

QFrame* LinuxMachinesSectionTest::row(QWidget& section, const QString& id) {
    for (auto* frame : section.findChildren<QFrame*>(QStringLiteral("linuxMachineRow")))
        if (frame->property("machineId").toString() == id)
            return frame;
    return nullptr;
}

ket::Dialog* LinuxMachinesSectionTest::connectDialog(QWidget& section) {
    return section.window()->findChild<ket::Dialog*>(QStringLiteral("linuxPairingDialog"));
}

QAbstractButton* LinuxMachinesSectionTest::button(QWidget* parent, const QString& objectName) {
    return parent ? parent->findChild<QAbstractButton*>(objectName) : nullptr;
}

QString LinuxMachinesSectionTest::text(QWidget* parent, const QString& objectName) {
    auto* label = parent ? parent->findChild<QLabel*>(objectName) : nullptr;
    return label ? label->text() : QString();
}

void LinuxMachinesSectionTest::validatesSshHosts_data() {
    QTest::addColumn<QString>("host");
    QTest::addColumn<bool>("valid");
    QTest::newRow("alias") << "build-node-02" << true;
    QTest::newRow("user@host") << "jay@build.example.com" << true;
    QTest::newRow("port") << "jay@10.0.0.4:2222" << true;
    QTest::newRow("ipv6") << "[fe80::1]:22" << true;
    QTest::newRow("empty") << "" << false;
    QTest::newRow("option injection") << "-oProxyCommand=touch" << false;
    QTest::newRow("space") << "build node" << false;
    QTest::newRow("port out of range") << "build:70000" << false;
    QTest::newRow("two users") << "a@b@c" << false;
}

void LinuxMachinesSectionTest::validatesSshHosts() {
    QFETCH(QString, host);
    QFETCH(bool, valid);
    QCOMPARE(validateSshHost(host).isEmpty(), valid);
}

void LinuxMachinesSectionTest::rowsShowStateRouteAndErrors() {
    LinuxMachinesSection section;
    section.resize(760, 720);
    section.show();
    section.model()->setBackendsAttached(true);
    remote::RemoteControlTestAccess::signIn(*section.machines_->client_);
    QSignalSpy changed(&section, &LinuxMachinesSection::machinesChanged);
    section.model()->setMachines(fleet());
    QCOMPARE(changed.size(), 1);
    // The picker shows the row's route and state.
    QCOMPARE(changed.first().at(1).toStringList().first(),
             QStringLiteral("build-node-01 · SSH · Connected"));
    QCOMPARE(section.findChild<QLabel*>(QStringLiteral("linuxMachinesCount"))->text(),
             QStringLiteral("5 trusted"));

    struct Expected {
        QString id, state, tone, route, toggle;
    };
    const QList<Expected> expected{
        {"ssh-1", "Connected", "positive", "SSH", "Disconnect"},
        {"relay-1", "Offline", "", "Relay", "Connect"},
        {"ssh-2", "Connecting…", "warning", "SSH", "Disconnect"},
        {"ssh-3", "Reconnecting…", "warning", "SSH", "Disconnect"},
        {"ssh-4", "Host key changed", "danger", "SSH", "Connect"},
    };
    for (const auto& want : expected) {
        auto* card = row(section, want.id);
        QVERIFY2(card, qPrintable(want.id));
        auto* state = card->findChild<QLabel*>(QStringLiteral("linuxMachineState"));
        QCOMPARE(state->text(), want.state);
        QCOMPARE(state->property("remoteTone").toString(), want.tone);
        QCOMPARE(text(card, QStringLiteral("linuxMachineRoute")), want.route);
        QCOMPARE(button(card, QStringLiteral("linuxMachineToggle"))->text(), want.toggle);
        QVERIFY(button(card, QStringLiteral("linuxMachineDetails")));
    }
    QCOMPARE(text(row(section, "ssh-1"), QStringLiteral("linuxMachineDetail")),
             QStringLiteral("Ubuntu 24.04 LTS · Ready for remote work"));
    QVERIFY(!row(section, "ssh-1")->findChild<QLabel*>(QStringLiteral("linuxMachineError")));
    // The error says what to do, on the row itself.
    const QString error = text(row(section, "ssh-4"), QStringLiteral("linuxMachineError"));
    QVERIFY2(error.contains(QStringLiteral("ssh-keygen -R old-host")), qPrintable(error));
    // No password field anywhere in the section.
    for (auto* field : section.findChildren<QLineEdit*>())
        QVERIFY(field->echoMode() != QLineEdit::Password);
}

void LinuxMachinesSectionTest::rowButtonsAndDetailsEmitIntents() {
    LinuxMachinesSection section;
    section.resize(760, 720);
    section.show();
    section.model()->setBackendsAttached(true);
    auto* model = section.model();
    model->setMachines(fleet());
    QSignalSpy connects(model, &LinuxMachineModel::connectRequested);
    QSignalSpy disconnects(model, &LinuxMachineModel::disconnectRequested);

    button(row(section, "relay-1"), QStringLiteral("linuxMachineToggle"))->click();
    QCOMPARE(connects.size(), 1);
    QCOMPARE(connects.takeFirst().first().toString(), QStringLiteral("relay-1"));
    button(row(section, "ssh-1"), QStringLiteral("linuxMachineToggle"))->click();
    QCOMPARE(disconnects.size(), 1);
    QCOMPARE(disconnects.takeFirst().first().toString(), QStringLiteral("ssh-1"));

    // Details: the route and the verified fingerprint, with Connect and Disconnect.
    button(row(section, "ssh-1"), QStringLiteral("linuxMachineDetails"))->click();
    auto* details =
        section.window()->findChild<ket::Dialog*>(QStringLiteral("linuxMachineDetailsDialog"));
    QVERIFY(details && details->isOpen());
    QCOMPARE(details->title(), QStringLiteral("build-node-01"));
    QVERIFY(text(details, QStringLiteral("linuxDetailsRoute")).startsWith(QStringLiteral("SSH")));
    QCOMPARE(text(details, QStringLiteral("linuxDetailsFingerprint")), fleet().first().fingerprint);
    QCOMPARE(text(details, QStringLiteral("linuxDetailsVerified")), QStringLiteral("Verified"));
    QVERIFY(!button(details, QStringLiteral("linuxDetailsConnect"))->isEnabled());
    auto* disconnect = button(details, QStringLiteral("linuxDetailsDisconnect"));
    QVERIFY(disconnect->isEnabled());
    disconnect->click();
    QCOMPARE(disconnects.size(), 1);
    QCOMPARE(disconnects.takeFirst().first().toString(), QStringLiteral("ssh-1"));
    QTRY_VERIFY(
        !section.window()->findChild<ket::Dialog*>(QStringLiteral("linuxMachineDetailsDialog")));

    // A machine with an error: no verified key yet, the fix, and Connect.
    button(row(section, "ssh-4"), QStringLiteral("linuxMachineDetails"))->click();
    details =
        section.window()->findChild<ket::Dialog*>(QStringLiteral("linuxMachineDetailsDialog"));
    QVERIFY(details);
    QCOMPARE(text(details, QStringLiteral("linuxDetailsFingerprint")),
             QStringLiteral("Not verified yet"));
    QVERIFY(text(details, QStringLiteral("linuxDetailsError")).contains("known_hosts"));
    auto* connect = button(details, QStringLiteral("linuxDetailsConnect"));
    QVERIFY(connect->isEnabled());
    connect->click();
    QCOMPARE(connects.size(), 1);
    QCOMPARE(connects.takeFirst().first().toString(), QStringLiteral("ssh-4"));
}

void LinuxMachinesSectionTest::sshModeAcceptsAndRejectsAFirstUseHostKey() {
    LinuxMachinesSection section;
    section.resize(760, 720);
    section.show();
    section.model()->setBackendsAttached(true);
    auto* model = section.model();
    QSignalSpy requests(model, &LinuxMachineModel::sshConnectRequested);
    QSignalSpy decisions(model, &LinuxMachineModel::hostKeyDecided);
    QSignalSpy cancels(model, &LinuxMachineModel::sshConnectCancelled);

    // Signed out: SSH needs no account, so the card opens on its SSH mode.
    section.beginPairing();
    auto* dialog = connectDialog(section);
    QVERIFY(dialog && dialog->isOpen());
    QCOMPARE(dialog->title(), QStringLiteral("Connect a Linux machine"));
    auto* modes = dialog->findChild<ket::SegmentedControl*>(QStringLiteral("linuxConnectMode"));
    QVERIFY(modes);
    QCOMPARE(modes->currentIndex(), 0);
    QVERIFY(dialog->findChild<QWidget*>(QStringLiteral("linuxSshPage"))->isVisible());
    QVERIFY(!dialog->findChild<QWidget*>(QStringLiteral("linuxRelayPage"))->isVisible());
    auto* host = dialog->findChild<QLineEdit*>(QStringLiteral("linuxSshHost"));
    QCOMPARE(host->echoMode(), QLineEdit::Normal);
    // No password field in the SSH mode.
    for (auto* field :
         dialog->findChild<QWidget*>(QStringLiteral("linuxSshPage"))->findChildren<QLineEdit*>())
        QVERIFY(field->echoMode() != QLineEdit::Password);
    auto* connect = button(dialog, QStringLiteral("linuxSshConnect"));
    auto* feedback = dialog->findChild<QLabel*>(QStringLiteral("linuxSshFeedback"));

    // A value ssh would read as an option is refused before anything runs.
    host->setText(QStringLiteral("-oProxyCommand=touch"));
    connect->click();
    QCOMPARE(requests.size(), 0);
    QVERIFY(!feedback->isHidden());
    QCOMPARE(feedback->property("remoteTone").toString(), QStringLiteral("danger"));

    host->setText(QStringLiteral("jay@build-node-03:2222"));
    connect->click();
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.takeFirst().first().toString(), QStringLiteral("jay@build-node-03:2222"));
    QVERIFY(!connect->isEnabled());
    QVERIFY(feedback->text().contains(QStringLiteral("Connecting")));

    // A key for another host is not this attempt's.
    auto* hostKey = dialog->findChild<QFrame*>(QStringLiteral("linuxSshHostKey"));
    model->presentHostKey(QStringLiteral("other"), QStringLiteral("SHA256:other"));
    QVERIFY(hostKey->isHidden());
    const QString fingerprint = QStringLiteral("SHA256:Zx9yW8vU7tS6rQ5pO4nM3lK2jI1hG0fE9dC8bA7");
    model->presentHostKey(QStringLiteral("jay@build-node-03:2222"), fingerprint);
    QVERIFY(hostKey->isVisible());
    QCOMPARE(text(hostKey, QStringLiteral("linuxSshFingerprint")), fingerprint);
    QVERIFY(text(hostKey, QStringLiteral("linuxSshHostKeyPrompt")).contains("first connection"));

    button(hostKey, QStringLiteral("linuxSshRejectKey"))->click();
    QCOMPARE(decisions.size(), 1);
    auto decision = decisions.takeFirst();
    QCOMPARE(decision.at(1).toString(), fingerprint);
    QCOMPARE(decision.at(2).toBool(), false);
    QVERIFY(hostKey->isHidden());
    QVERIFY(connect->isEnabled());
    QVERIFY(feedback->text().contains(QStringLiteral("rejected")));

    connect->click();
    model->presentHostKey(QStringLiteral("jay@build-node-03:2222"), fingerprint);
    button(hostKey, QStringLiteral("linuxSshAcceptKey"))->click();
    QCOMPARE(decisions.size(), 1);
    decision = decisions.takeFirst();
    QCOMPARE(decision.at(0).toString(), QStringLiteral("jay@build-node-03:2222"));
    QCOMPARE(decision.at(2).toBool(), true);
    QVERIFY(hostKey->isHidden());

    model->reportSshConnected(QStringLiteral("jay@build-node-03:2222"));
    QTRY_VERIFY(!connectDialog(section));
    QCOMPARE(cancels.size(), 0);
    auto* notice = section.findChild<QLabel*>(QStringLiteral("linuxMachinesNotice"));
    QVERIFY(notice->text().contains(QStringLiteral("connected over SSH")));

    // Closing the card during an attempt cancels it.
    section.beginPairing();
    dialog = connectDialog(section);
    dialog->findChild<QLineEdit*>(QStringLiteral("linuxSshHost"))->setText("build-node-04");
    button(dialog, QStringLiteral("linuxSshConnect"))->click();
    dialog->reject();
    QCOMPARE(cancels.size(), 1);
    QCOMPARE(cancels.first().first().toString(), QStringLiteral("build-node-04"));
}

void LinuxMachinesSectionTest::sshFailuresAreActionable() {
    LinuxMachinesSection section;
    section.resize(760, 720);
    section.show();
    section.model()->setBackendsAttached(true);
    auto* model = section.model();
    section.beginPairing();
    auto* dialog = connectDialog(section);
    QVERIFY(dialog);
    auto* host = dialog->findChild<QLineEdit*>(QStringLiteral("linuxSshHost"));
    auto* connect = button(dialog, QStringLiteral("linuxSshConnect"));
    auto* feedback = dialog->findChild<QLabel*>(QStringLiteral("linuxSshFeedback"));
    const QList<std::pair<Error, QString>> failures{
        {Error::HostKeyMismatch, QStringLiteral("does not match the one in known_hosts")},
        {Error::AuthenticationFailed, QStringLiteral("ssh-add")},
        {Error::DaemonNotInstalled, QStringLiteral("ketPlus daemon is not installed on")},
        {Error::Other, QStringLiteral("Connection timed out")},
    };
    for (const auto& [error, phrase] : failures) {
        host->setText(QStringLiteral("build-node-05"));
        connect->click();
        QVERIFY(!connect->isEnabled());
        model->reportSshFailure(QStringLiteral("build-node-05"), error,
                                QStringLiteral("Connection timed out"));
        QVERIFY2(feedback->text().contains(phrase), qPrintable(feedback->text()));
        QCOMPARE(feedback->property("remoteTone").toString(), QStringLiteral("danger"));
        QVERIFY(connect->isEnabled());
        QVERIFY(host->isEnabled());
    }
    QVERIFY(LinuxMachineModel::errorMessage(Error::AuthenticationFailed, "h")
                .contains(QStringLiteral("does not use SSH passwords")));
    dialog->reject();
}

void LinuxMachinesSectionTest::relayModeOpensPairingAndRespectsTheEntitlement() {
    LinuxMachinesSection section;
    section.resize(760, 720);
    section.show();
    section.model()->setBackendsAttached(true);
    const auto open = [&section] {
        section.beginPairing();
        auto* dialog = connectDialog(section);
        auto* modes = dialog->findChild<ket::SegmentedControl*>(QStringLiteral("linuxConnectMode"));
        modes->setCurrentIndex(1);
        emit modes->currentChanged(1);
        return dialog;
    };
    const auto checkLink = [](ket::Dialog* dialog) -> QAbstractButton* {
        for (auto* candidate : dialog->findChildren<QAbstractButton*>())
            if (candidate->text() == QStringLiteral("Check link"))
                return candidate;
        return nullptr;
    };

    // Signed out: the Relay mode asks for the account instead of a link.
    auto* dialog = open();
    auto* relay = dialog->findChild<QWidget*>(QStringLiteral("linuxRelayPage"));
    QVERIFY(relay->isVisible());
    QVERIFY(!dialog->findChild<QWidget*>(QStringLiteral("linuxSshPage"))->isVisible());
    QVERIFY(!button(dialog, QStringLiteral("linuxSshConnect"))->isVisible());
    QVERIFY(button(relay, QStringLiteral("linuxRelaySignIn"))->isVisible());
    QVERIFY(!relay->findChild<QLineEdit*>(QStringLiteral("linuxPairingLink"))->isVisible());
    QVERIFY(!checkLink(dialog)->isEnabled());
    dialog->reject();
    QTRY_VERIFY(!connectDialog(section));

    // Signed in with the entitlement: the existing pairing link flow.
    remote::RemoteControlTestAccess::signIn(*section.machines_->client_);
    remote::RemoteControlTestAccess::entitle(*section.machines_->client_,
                                             QJsonArray{QString::fromLatin1(relayEntitlementName)});
    section.machines_->syncEntitlement();
    QVERIFY(section.model()->relayEntitled());
    dialog = open();
    relay = dialog->findChild<QWidget*>(QStringLiteral("linuxRelayPage"));
    QVERIFY(!relay->findChild<QLabel*>(QStringLiteral("linuxRelayGate"))->isVisible());
    auto* link = relay->findChild<QLineEdit*>(QStringLiteral("linuxPairingLink"));
    QVERIFY(link->isVisible());
    auto* check = checkLink(dialog);
    QVERIFY(check && check->isVisible() && check->isEnabled());
    link->setText(QStringLiteral("https://example.com"));
    check->click();
    auto* feedback = relay->findChild<QLabel*>(QStringLiteral("linuxPairingFeedback"));
    QVERIFY(feedback->text().contains(QStringLiteral("pairing link")));

    // Without ketPlus Pro the Relay explains itself and offers no pairing.
    section.model()->setRelayEntitled(false);
    auto* gate = relay->findChild<QLabel*>(QStringLiteral("linuxRelayGate"));
    QVERIFY(gate->isVisible());
    QVERIFY(gate->text().contains(QStringLiteral("ketPlus Pro")));
    QVERIFY(!link->isVisible());
    QVERIFY(!check->isEnabled());
    QVERIFY(!button(relay, QStringLiteral("linuxRelaySignIn"))->isVisible());
    dialog->reject();
}

void LinuxMachinesSectionTest::relayRefreshKeepsSshMachines() {
    // ADR 0119: machines are keyed by the host key fingerprint; the relay id is an attribute.
    const QString sshKey = QStringLiteral("SHA256:ssh-host");
    const QString relayKey = QStringLiteral("SHA256:relay-host");
    auto reported =
        make(relayKey, "build-node-02", "Linux · Trusted", Route::Relay, State::Offline, relayKey);
    reported.relayId = QStringLiteral("r-1");
    LinuxMachineModel model;
    model.setMachines(
        {make(sshKey, "build-node-01", "Ubuntu", Route::Ssh, State::Connected, sshKey), reported});
    model.setRelayMachines({{QStringLiteral("r-1"), relayKey},
                            {QStringLiteral("r-2"), sshKey},
                            {QStringLiteral("r-3"), {}},
                            {QStringLiteral("r-3"), {}},
                            {{}, QStringLiteral("SHA256:no-relay-id")}});
    QStringList ids;
    for (const auto& machine : model.machines())
        ids.append(machine.id);
    QCOMPARE(ids, (QStringList{sshKey, relayKey, QStringLiteral("relay:r-3")}));
    // The host the Relay also lists is still the one SSH machine, with the relay id beside it.
    QCOMPARE(model.machine(sshKey)->route, Route::Ssh);
    QCOMPARE(model.machine(sshKey)->relayId, QStringLiteral("r-2"));
    // A known Relay machine keeps what the backend reported about it.
    QCOMPARE(model.machine(relayKey)->state, State::Offline);
    QCOMPARE(model.machine(relayKey)->relayId, QStringLiteral("r-1"));
    // Paired before host pins: no verified identity yet, so it cannot merge with anything.
    const auto* unpinned = model.machine(QStringLiteral("relay:r-3"));
    QCOMPARE(unpinned->route, Route::Relay);
    QCOMPARE(unpinned->state, State::Connected);
    QVERIFY(unpinned->fingerprint.isEmpty());
    QCOMPARE(unpinned->name, QStringLiteral("r-3"));

    model.setRelayMachines({});
    QCOMPARE(model.machines().size(), 1);
    QVERIFY(model.machine(sshKey)->relayId.isEmpty());
}

void LinuxMachinesSectionTest::pinnedFingerprintNamesTheRelayHost() {
    // The section's own client, as the other tests use: no second credential store.
    LinuxMachinesSection section;
    auto& client = *section.machines_->client_;
    remote::RemoteControlTestAccess::pin(
        client, {{QStringLiteral("m1/1"), QStringLiteral("SHA256:old")},
                 {QStringLiteral("m1/2"), QStringLiteral("SHA256:new")},
                 {QStringLiteral("m10/3"), QStringLiteral("SHA256:other")},
                 {QStringLiteral("a/b/1"), QStringLiteral("SHA256:slash")}});
    QCOMPARE(client.pinnedFingerprint(QStringLiteral("m1")), QStringLiteral("SHA256:new"));
    QCOMPARE(client.pinnedFingerprint(QStringLiteral("m10")), QStringLiteral("SHA256:other"));
    QCOMPARE(client.pinnedFingerprint(QStringLiteral("a/b")), QStringLiteral("SHA256:slash"));
    QVERIFY(client.pinnedFingerprint(QStringLiteral("m")).isEmpty());
}

void LinuxMachinesSectionTest::withoutBackendsOnlyTheRelayIsOffered() {
    LinuxMachinesSection section;
    section.resize(760, 720);
    section.show();
    remote::RemoteControlTestAccess::signIn(*section.machines_->client_);
    QVERIFY(!section.model()->backendsAttached());
    section.model()->setRelayMachines({{QStringLiteral("relay-1"), {}}});
    // As before the model: listed by the Relay, "Available", nothing to connect or disconnect.
    auto* card = row(section, "relay:relay-1");
    QVERIFY(card);
    QCOMPARE(text(card, QStringLiteral("linuxMachineState")), QStringLiteral("Available"));
    QVERIFY(!button(card, QStringLiteral("linuxMachineToggle")));
    button(card, QStringLiteral("linuxMachineDetails"))->click();
    auto* details =
        section.window()->findChild<ket::Dialog*>(QStringLiteral("linuxMachineDetailsDialog"));
    QVERIFY(details && details->isOpen());
    QCOMPARE(text(details, QStringLiteral("linuxDetailsState")), QStringLiteral("Available"));
    QVERIFY(!button(details, QStringLiteral("linuxDetailsConnect")));
    QVERIFY(!button(details, QStringLiteral("linuxDetailsDisconnect")));
    details->reject();
    QTRY_VERIFY(
        !section.window()->findChild<ket::Dialog*>(QStringLiteral("linuxMachineDetailsDialog")));

    // The card offers the Relay only: no SSH mode would be answered.
    section.beginPairing();
    auto* dialog = connectDialog(section);
    QVERIFY(dialog && dialog->isOpen());
    QVERIFY(!dialog->findChild<ket::SegmentedControl*>(QStringLiteral("linuxConnectMode"))
                 ->isVisible());
    QVERIFY(!dialog->findChild<QWidget*>(QStringLiteral("linuxSshPage"))->isVisible());
    QVERIFY(dialog->findChild<QWidget*>(QStringLiteral("linuxRelayPage"))->isVisible());
    dialog->reject();
    QTRY_VERIFY(!connectDialog(section));

    // Attaching the backends brings SSH and the row actions back.
    section.model()->setBackendsAttached(true);
    card = row(section, "relay:relay-1");
    QCOMPARE(text(card, QStringLiteral("linuxMachineState")), QStringLiteral("Connected"));
    QVERIFY(button(card, QStringLiteral("linuxMachineToggle")));
}

void LinuxMachinesSectionTest::signedOutOffersSshWithoutAnAccount() {
    LinuxMachinesSection section;
    section.resize(760, 720);
    section.setPairingEntryShown(true);
    section.show();
    const auto entry = [&section] {
        return section.findChild<QAbstractButton*>(QStringLiteral("linuxMachinesConnect"));
    };
    const auto signIn = [&section] {
        return section.findChild<QAbstractButton*>(QStringLiteral("linuxMachinesSignIn"));
    };
    const auto messages = [&section] {
        QString all;
        for (auto* label : section.findChildren<QLabel*>())
            if (label->property("remoteRole").toString() == QStringLiteral("emptyText"))
                all += label->text();
        return all;
    };
    // Without backends: the Relay's sign-in only, with the account term of the page above.
    QVERIFY(!entry());
    QVERIFY(signIn());
    QVERIFY(messages().contains(QStringLiteral("KetSuite account")));

    section.model()->setBackendsAttached(true);
    QVERIFY(entry());
    QVERIFY(signIn());
    QVERIFY2(messages().contains(QStringLiteral("SSH")), qPrintable(messages()));
    QVERIFY(messages().contains(QStringLiteral("no account")));
    entry()->click();
    auto* dialog = connectDialog(section);
    QVERIFY(dialog && dialog->isOpen());
    QVERIFY(dialog->findChild<QWidget*>(QStringLiteral("linuxSshPage"))->isVisible());
    dialog->reject();
    QTRY_VERIFY(!connectDialog(section));

    // Signed in with no machines, the empty state names both routes.
    remote::RemoteControlTestAccess::signIn(*section.machines_->client_);
    section.render();
    QVERIFY2(messages().contains(QStringLiteral("SSH")), qPrintable(messages()));
    QVERIFY(messages().contains(QStringLiteral("Relay")));
    QVERIFY(entry());
}

void LinuxMachinesSectionTest::detailsFollowTheModel() {
    LinuxMachinesSection section;
    section.resize(760, 720);
    section.show();
    auto* model = section.model();
    model->setBackendsAttached(true);
    model->setMachines(fleet());
    button(row(section, "ssh-1"), QStringLiteral("linuxMachineDetails"))->click();
    auto* details =
        section.window()->findChild<ket::Dialog*>(QStringLiteral("linuxMachineDetailsDialog"));
    QVERIFY(details && details->isOpen());
    QCOMPARE(text(details, QStringLiteral("linuxDetailsState")), QStringLiteral("Connected"));
    auto* error = details->findChild<QLabel*>(QStringLiteral("linuxDetailsError"));
    QVERIFY(error->isHidden());
    QVERIFY(button(details, QStringLiteral("linuxDetailsDisconnect"))->isEnabled());

    // The machine drops while the card is open: the card says so and offers Connect.
    auto changed = fleet().first();
    changed.state = State::Offline;
    changed.error = Error::AuthenticationFailed;
    model->upsert(changed);
    QCOMPARE(text(details, QStringLiteral("linuxDetailsState")), QStringLiteral("SSH key refused"));
    QVERIFY(!error->isHidden());
    QVERIFY(error->text().contains(QStringLiteral("ssh-add")));
    QVERIFY(button(details, QStringLiteral("linuxDetailsConnect"))->isEnabled());
    QVERIFY(!button(details, QStringLiteral("linuxDetailsDisconnect"))->isEnabled());

    // The machine is removed: the card closes rather than describe a machine that is gone.
    model->setMachines({});
    QTRY_VERIFY(
        !section.window()->findChild<ket::Dialog*>(QStringLiteral("linuxMachineDetailsDialog")));
}

void LinuxMachinesSectionTest::enterConnectsAndAHostKeyFocusesReject() {
    LinuxMachinesSection section;
    section.resize(760, 720);
    section.show();
    QVERIFY(QTest::qWaitForWindowActive(&section));
    auto* model = section.model();
    model->setBackendsAttached(true);
    QSignalSpy requests(model, &LinuxMachineModel::sshConnectRequested);
    section.beginPairing();
    auto* dialog = connectDialog(section);
    QVERIFY(dialog);
    auto* host = dialog->findChild<QLineEdit*>(QStringLiteral("linuxSshHost"));
    host->setText(QStringLiteral("build-node-06"));
    QTest::keyClick(host, Qt::Key_Return);
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().first().toString(), QStringLiteral("build-node-06"));
    // A second Enter while connecting asks nothing more.
    QTest::keyClick(host, Qt::Key_Return);
    QCOMPARE(requests.size(), 1);
    model->presentHostKey(QStringLiteral("build-node-06"), QStringLiteral("SHA256:abc"));
    QTRY_VERIFY(button(dialog, QStringLiteral("linuxSshRejectKey"))->hasFocus());
    dialog->reject();
}

void LinuxMachinesSectionTest::detailsShowAnOfferedKeyAsAMismatchAndOfferRetrust() {
    LinuxMachinesSection section;
    section.resize(760, 720);
    section.show();
    auto* model = section.model();
    model->setBackendsAttached(true);
    auto changed = make("ssh-4", "old-host", "Linux · old-host", Route::Ssh, State::Offline, {},
                        Error::HostKeyMismatch);
    changed.presentedFingerprint = QStringLiteral("SHA256:offeredKeyThatDidNotVerify");
    changed.canForgetHostKey = true;
    model->setMachines({changed});
    QSignalSpy forget(model, &LinuxMachineModel::forgetHostKeyRequested);
    QSignalSpy removed(model, &LinuxMachineModel::removeRequested);
    // The row explains the ketPlus re-trust path instead of ssh-keygen -R.
    QVERIFY(text(row(section, "ssh-4"), QStringLiteral("linuxMachineError"))
                .contains(QStringLiteral("Forget old key")));
    button(row(section, "ssh-4"), QStringLiteral("linuxMachineDetails"))->click();
    auto* details =
        section.window()->findChild<ket::Dialog*>(QStringLiteral("linuxMachineDetailsDialog"));
    QVERIFY(details && details->isOpen());
    QCOMPARE(text(details, QStringLiteral("linuxDetailsFingerprint")),
             changed.presentedFingerprint);
    QCOMPARE(text(details, QStringLiteral("linuxDetailsVerified")),
             QStringLiteral("Does not match the key accepted before"));
    QVERIFY(!button(details, QStringLiteral("linuxDetailsForgetKey"))->isHidden());
    QVERIFY(button(details, QStringLiteral("linuxDetailsTrustIdentity"))->isHidden());
    button(details, QStringLiteral("linuxDetailsForgetKey"))->click();
    QCOMPARE(forget.size(), 1);
    QCOMPARE(forget.first().first().toString(), QStringLiteral("ssh-4"));

    QTRY_VERIFY(
        !section.window()->findChild<ket::Dialog*>(QStringLiteral("linuxMachineDetailsDialog")));

    // A verified machine reads Verified and offers Remove; no re-trust is offered.
    model->setMachines(fleet());
    button(row(section, "ssh-1"), QStringLiteral("linuxMachineDetails"))->click();
    details =
        section.window()->findChild<ket::Dialog*>(QStringLiteral("linuxMachineDetailsDialog"));
    QVERIFY(details && details->isOpen());
    QCOMPARE(text(details, QStringLiteral("linuxDetailsVerified")), QStringLiteral("Verified"));
    QVERIFY(button(details, QStringLiteral("linuxDetailsForgetKey"))->isHidden());
    button(details, QStringLiteral("linuxDetailsRemove"))->click();
    QCOMPARE(removed.size(), 1);
    QCOMPARE(removed.first().first().toString(), QStringLiteral("ssh-1"));
}

void LinuxMachinesSectionTest::theRelayStopsWhenTheSessionIsRevoked() {
    LinuxMachinesSection section;
    QPointer<testing::FakeRelayLink> link;
    section.attachRelayBackend([&link](const QString& relayId) {
        auto made = std::make_unique<testing::FakeRelayLink>(relayId);
        link = made.get();
        return made;
    });
    remote::RemoteControlTestAccess::signIn(*section.machines_->client_);
    remote::RemoteControlTestAccess::entitle(*section.machines_->client_,
                                             QJsonArray{QString::fromLatin1(relayEntitlementName)});
    section.machines_->syncEntitlement();
    section.machines_->relay_->setListed(
        {{QStringLiteral("relay-1"), QStringLiteral("SHA256:pinned"), QStringLiteral("builder"),
          QDateTime::currentDateTimeUtc()}});
    QVERIFY(link);
    QCOMPARE(link->connects, 1);
    QCOMPARE(section.model()->machines().first().state, State::Connected);
    QCOMPARE(section.machineBackend(QStringLiteral("SHA256:pinned")), link->backend());
    // The relay refused the refresh credential: signed out, so the Relay stops.
    remote::RemoteControlTestAccess::signOut(*section.machines_->client_);
    emit section.machines_->client_->credentialRevoked();
    QCOMPARE(link->disconnects, 1);
    QVERIFY(!section.model()->relayEntitled());
    QVERIFY(!section.model()->machines().first().connectable);
}

void LinuxMachinesSectionTest::anIdentityChangeShowsBothIdentitiesBeforeTrust() {
    LinuxMachinesSection section;
    section.resize(760, 720);
    section.show();
    auto* model = section.model();
    model->setBackendsAttached(true);
    auto rotated = make("SHA256:trustedDaemonKey", "build-node-02", "Linux · build-node-02",
                        Route::Ssh, State::Offline, {}, Error::IdentityChanged);
    rotated.presentedIdentity = QStringLiteral("SHA256:presentedDaemonKey");
    model->setMachines({rotated});
    QSignalSpy trusted(model, &LinuxMachineModel::trustIdentityRequested);
    button(row(section, "SHA256:trustedDaemonKey"), QStringLiteral("linuxMachineDetails"))->click();
    auto* details =
        section.window()->findChild<ket::Dialog*>(QStringLiteral("linuxMachineDetailsDialog"));
    QVERIFY(details && details->isOpen());
    QCOMPARE(text(details, QStringLiteral("linuxDetailsTrustedIdentity")),
             QStringLiteral("SHA256:trustedDaemonKey"));
    QCOMPARE(text(details, QStringLiteral("linuxDetailsPresentedIdentity")),
             QStringLiteral("SHA256:presentedDaemonKey"));
    auto* trust = button(details, QStringLiteral("linuxDetailsTrustIdentity"));
    QVERIFY(!trust->isHidden() && trust->isEnabled());

    // A daemon that names no identity offers nothing to trust.
    rotated.presentedIdentity.clear();
    model->setMachines({rotated});
    QCOMPARE(text(details, QStringLiteral("linuxDetailsPresentedIdentity")),
             QStringLiteral("None: the daemon named no identity"));
    QVERIFY(!trust->isEnabled());
    rotated.presentedIdentity = QStringLiteral("SHA256:presentedDaemonKey");
    model->setMachines({rotated});
    trust->click();
    QCOMPARE(trusted.size(), 1);
    QTRY_VERIFY(
        !section.window()->findChild<ket::Dialog*>(QStringLiteral("linuxMachineDetailsDialog")));

    // Without a rotation, neither identity row shows.
    model->setMachines(fleet());
    button(row(section, "ssh-1"), QStringLiteral("linuxMachineDetails"))->click();
    details =
        section.window()->findChild<ket::Dialog*>(QStringLiteral("linuxMachineDetailsDialog"));
    QVERIFY(details);
    QVERIFY(
        !details->findChild<QLabel*>(QStringLiteral("linuxDetailsTrustedIdentity"))->isVisible());
    details->reject();
}

void LinuxMachinesSectionTest::theRelayFollowsTheAccountEntitlement() {
    // The policy for a session that names no entitlements: D-4 fails closed; Allow is the switch.
    QVERIFY(!relayEntitled(std::nullopt));
    QVERIFY(!relayEntitled(std::nullopt, MissingEntitlements::Deny));
    QVERIFY(relayEntitled(std::nullopt, MissingEntitlements::Allow));
    QVERIFY(relayEntitled(QStringList{QString::fromLatin1(relayEntitlementName)},
                          MissingEntitlements::Deny));
    QVERIFY(
        !relayEntitled(QStringList{QStringLiteral("ketrouter.usage")}, MissingEntitlements::Allow));

    LinuxMachinesSection section;
    section.resize(760, 720);
    section.show();
    auto* model = section.model();
    model->setBackendsAttached(true);
    // Signed out or signed in without a named entitlement: not entitled (no default-true).
    QVERIFY(!model->relayEntitled());
    remote::RemoteControlTestAccess::signIn(*section.machines_->client_);
    section.machines_->syncEntitlement();
    QVERIFY(!model->relayEntitled());
    remote::RemoteControlTestAccess::entitle(*section.machines_->client_,
                                             QJsonArray{QStringLiteral("ketrouter.usage")});
    section.machines_->syncEntitlement();
    QVERIFY(!model->relayEntitled());

    // Not Pro: the Relay mode shows the upgrade hint and no pairing; SSH stays offered.
    section.beginPairing();
    auto* dialog = connectDialog(section);
    QVERIFY(dialog);
    QVERIFY(dialog->findChild<QWidget*>(QStringLiteral("linuxSshPage"))->isVisible());
    QVERIFY(button(dialog, QStringLiteral("linuxSshConnect"))->isEnabled());
    auto* modes = dialog->findChild<ket::SegmentedControl*>(QStringLiteral("linuxConnectMode"));
    modes->setCurrentIndex(1);
    emit modes->currentChanged(1);
    auto* relay = dialog->findChild<QWidget*>(QStringLiteral("linuxRelayPage"));
    auto* gate = relay->findChild<QLabel*>(QStringLiteral("linuxRelayGate"));
    QVERIFY(gate->isVisible());
    QVERIFY(gate->text().contains(QStringLiteral("upgrade your KetSuite account")));
    QVERIFY(!relay->findChild<QLineEdit*>(QStringLiteral("linuxPairingLink"))->isVisible());

    // The account becomes Pro: the open card offers pairing.
    remote::RemoteControlTestAccess::entitle(*section.machines_->client_,
                                             QJsonArray{QString::fromLatin1(relayEntitlementName)});
    section.machines_->syncEntitlement();
    QVERIFY(model->relayEntitled());
    QVERIFY(!gate->isVisible());
    QVERIFY(relay->findChild<QLineEdit*>(QStringLiteral("linuxPairingLink"))->isVisible());
    dialog->reject();
}

void LinuxMachinesSectionTest::aForgottenKeyReopensTheCardOnThatHost() {
    LinuxMachinesSection section;
    section.resize(760, 720);
    section.show();
    auto* model = section.model();
    model->setBackendsAttached(true);
    QSignalSpy requests(model, &LinuxMachineModel::sshConnectRequested);
    model->promptSshConnect(QStringLiteral("dev@old-host:2222"));
    auto* dialog = connectDialog(section);
    QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QLineEdit*>(QStringLiteral("linuxSshHost"))->text(),
             QStringLiteral("dev@old-host:2222"));
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().first().toString(), QStringLiteral("dev@old-host:2222"));
    dialog->reject();
}

void LinuxMachinesSectionTest::savedMachinesConnectAtLaunchBeforeTheCardIsShown() {
    QTemporaryDir directory;
    const QString daemonName =
        QStringLiteral("ketplus-launch-%1").arg(QCoreApplication::applicationPid());
    remote::ssh::testing::FakeDaemon daemon(daemonName);
    const QString store = directory.filePath(QStringLiteral("machines.json"));
    const QString known = directory.filePath(QStringLiteral("known_hosts"));
    const auto sshLinks =
        [daemonName,
         known](const remote::ssh::SshTarget&) -> std::unique_ptr<remote::ssh::SshLink> {
        auto link = std::make_unique<remote::ssh::testing::FakeLink>(daemonName);
        link->knownHosts = known;
        return link;
    };
    {
        // A machine connected in an earlier run is saved.
        LinuxMachinesSection earlier;
        earlier.attachSshBackend(store, sshLinks);
        earlier.model()->requestSshConnect(QStringLiteral("build-node-01"));
        QTRY_VERIFY(earlier.model()->machines().size() == 1 &&
                    earlier.model()->machines().first().state == State::Connected);
    }

    // The next launch: the window starts the section without showing Connections.
    LinuxMachinesSection section;
    QSignalSpy picker(&section, &LinuxMachinesSection::machinesChanged);
    QPointer<testing::FakeRelayLink> relayLink;
    section.attachSshBackend(store, sshLinks);
    section.attachRelayBackend([&relayLink](const QString& relayId) {
        auto made = std::make_unique<testing::FakeRelayLink>(relayId);
        relayLink = made.get();
        return made;
    });
    remote::RemoteControlTestAccess::signIn(*section.machines_->client_);
    section.machines_->loading_ = true; // keeps the Relay list request off the network
    section.activate();
    QVERIFY(!section.isVisible());
    QTRY_VERIFY(!picker.isEmpty() &&
                picker.last().at(1).toStringList() ==
                    QStringList{QStringLiteral("build-node-01 · SSH · Connected")});

    // Without ketPlus Pro the Relay hosts are listed but not attached.
    section.machines_->relay_->setListed(
        {{QStringLiteral("relay-1"), QStringLiteral("SHA256:pinned"), QStringLiteral("builder"),
          QDateTime::currentDateTimeUtc()}});
    QVERIFY(!relayLink || relayLink->connects == 0);
    // Entitled, the launch attaches and connects them too.
    remote::RemoteControlTestAccess::entitle(*section.machines_->client_,
                                             QJsonArray{QString::fromLatin1(relayEntitlementName)});
    section.activate();
    QTRY_VERIFY(relayLink && relayLink->connects == 1);
    QTRY_VERIFY(
        picker.last().at(1).toStringList().contains(QStringLiteral("builder · Relay · Connected")));
    QVERIFY(!section.isVisible());
}

void LinuxMachinesSectionTest::aRelayFallbackRowSaysWhySshFailedAndOffersSsh() {
    LinuxMachinesSection section;
    section.resize(760, 720);
    section.show();
    auto* model = section.model();
    model->setBackendsAttached(true);
    auto fallback = make("SHA256:pinned", "build-node-02", "Linux · build-node-02", Route::Relay,
                         State::Connected);
    fallback.sshTarget = QStringLiteral("build-node-02");
    fallback.connectable = true;
    fallback.sshFailure = QStringLiteral("Connection timed out.");
    model->setMachines({fallback});
    QSignalSpy useSsh(model, &LinuxMachineModel::sshRouteRequested);
    auto* fallbackRow = row(section, "SHA256:pinned");
    QVERIFY(fallbackRow);
    QCOMPARE(text(fallbackRow, QStringLiteral("linuxMachineSshFailure")),
             QStringLiteral("Using the Relay because SSH failed. Connection timed out."));
    button(fallbackRow, QStringLiteral("linuxMachineUseSsh"))->click();
    QCOMPARE(useSsh.size(), 1);
    QCOMPARE(useSsh.first().first().toString(), QStringLiteral("SHA256:pinned"));

    // Disconnected, the Relay is not in use; Connect would use it.
    fallback.state = State::Offline;
    model->setMachines({fallback});
    QCOMPARE(text(row(section, "SHA256:pinned"), QStringLiteral("linuxMachineSshFailure")),
             QStringLiteral("SSH failed, so Connect uses the Relay. Connection timed out."));
    fallback.state = State::Connected;

    // A Relay row that is not standing in for SSH offers neither.
    fallback.sshFailure.clear();
    model->setMachines({fallback});
    fallbackRow = row(section, "SHA256:pinned");
    QVERIFY(!fallbackRow->findChild<QLabel*>(QStringLiteral("linuxMachineSshFailure")));
    QVERIFY(!button(fallbackRow, QStringLiteral("linuxMachineUseSsh")));
}

} // namespace ketplus

QTEST_MAIN(ketplus::LinuxMachinesSectionTest)
#include "LinuxMachinesSectionTest.moc"
