#include "remote/LinuxMachinesSection.h"
#include "control/ControlV2Client.h"
#include "remote/LinuxMachineModel.h"
#include "remote/LinuxMachineRow.h"

#include "KetButton.h"
#include "KetDialog.h"
#include "KetSegmentedControl.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTest>

namespace ketplus::remote {
class RemoteControlTestAccess {
  public:
    static void signIn(ControlV2Client& client) {
        client.credential_.insert(QStringLiteral("deviceId"), QStringLiteral("test-desktop"));
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
    remote::RemoteControlTestAccess::signIn(*section.client_);
    QSignalSpy changed(&section, &LinuxMachinesSection::machinesChanged);
    section.model()->setMachines(fleet());
    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed.first().at(1).toStringList().first(), QStringLiteral("build-node-01"));
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
    remote::RemoteControlTestAccess::signIn(*section.client_);
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
    LinuxMachineModel model;
    model.setMachines(fleet());
    model.setRelayMachines({QStringLiteral("relay-1"), QStringLiteral("relay-2"),
                            QStringLiteral("relay-2"), QString()});
    QStringList ids;
    for (const auto& machine : model.machines())
        ids.append(machine.id);
    QCOMPARE(ids, (QStringList{"ssh-1", "ssh-2", "ssh-3", "ssh-4", "relay-1", "relay-2"}));
    // A known Relay machine keeps what the backend reported about it.
    QCOMPARE(model.machine("relay-1")->state, State::Offline);
    QCOMPARE(model.machine("relay-2")->route, Route::Relay);
    QCOMPARE(model.machine("relay-2")->state, State::Connected);
    model.setRelayMachines({});
    QCOMPARE(model.machines().size(), 4);
}

} // namespace ketplus

QTEST_MAIN(ketplus::LinuxMachinesSectionTest)
#include "LinuxMachinesSectionTest.moc"
