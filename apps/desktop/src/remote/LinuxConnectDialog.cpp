#include "remote/LinuxConnectDialog.h"
#include "remote/LinuxDaemonInstallCard.h"

#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetProfile.h"
#include "KetSegmentedControl.h"
#include "KetTextField.h"
#include "control/ControlV2Client.h"
#include "remote/LinuxMachineModel.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QStyle>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <memory>

namespace ketplus {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("LinuxConnectDialog", text); }

void showFeedback(QLabel* feedback, const QString& text, const bool error = false) {
    feedback->setText(text);
    feedback->setProperty("remoteTone", error ? QStringLiteral("danger") : QString());
    feedback->style()->unpolish(feedback);
    feedback->style()->polish(feedback);
    feedback->setVisible(!text.isEmpty());
}

QLabel* feedbackLabel(const char* objectName, QWidget* parent) {
    auto* feedback = new QLabel(parent);
    feedback->setObjectName(QString::fromLatin1(objectName));
    feedback->setWordWrap(true);
    feedback->setTextFormat(Qt::PlainText);
    feedback->setTextInteractionFlags(Qt::TextSelectableByMouse);
    feedback->hide(); // no gap under the field until there is something to say
    return feedback;
}

// The SSH page: host, then a first-use host key to accept or reject.
struct SshPage final {
    QWidget* page;
    ket::TextField* host;
    QLabel* feedback;
    QFrame* hostKey;
    QLabel* hostKeyPrompt;
    QLabel* fingerprint;
    ket::Button* accept;
    ket::Button* reject;
};

SshPage makeSshPage(QWidget* parent) {
    SshPage ssh{};
    ssh.page = new QWidget(parent);
    ssh.page->setObjectName(QStringLiteral("linuxSshPage"));
    auto* layout = new QVBoxLayout(ssh.page);
    layout->setContentsMargins(QMargins());
    layout->setSpacing(12);
    ssh.host = new ket::TextField(ssh.page);
    ssh.host->setObjectName(QStringLiteral("linuxSshHost"));
    ssh.host->setAccessibleName(tr("SSH host"));
    ssh.host->setPlaceholderText(QStringLiteral("build-node-02 or user@host:22"));
    auto* row = new ket::FieldRow(tr("Host"), ssh.host,
                                  tr("A host from ~/.ssh/config or user@host[:port]. ketPlus uses "
                                     "your SSH keys, agent and known_hosts and never asks for a "
                                     "password."),
                                  ssh.page);
    row->setObjectName(QStringLiteral("linuxSshRow"));
    layout->addWidget(row);
    ssh.feedback = feedbackLabel("linuxSshFeedback", ssh.page);
    layout->addWidget(ssh.feedback);
    ssh.hostKey = new QFrame(ssh.page);
    ssh.hostKey->setObjectName(QStringLiteral("linuxSshHostKey"));
    ssh.hostKey->setProperty("remoteRole", QStringLiteral("surface"));
    auto* keyLayout = new QVBoxLayout(ssh.hostKey);
    keyLayout->setContentsMargins(16, 14, 16, 14);
    keyLayout->setSpacing(10);
    ssh.hostKeyPrompt = new QLabel(ssh.hostKey);
    ssh.hostKeyPrompt->setObjectName(QStringLiteral("linuxSshHostKeyPrompt"));
    ssh.hostKeyPrompt->setWordWrap(true);
    keyLayout->addWidget(ssh.hostKeyPrompt);
    ssh.fingerprint = new QLabel(ssh.hostKey);
    ssh.fingerprint->setObjectName(QStringLiteral("linuxSshFingerprint"));
    ssh.fingerprint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont mono = ssh.fingerprint->font();
    if (const QString family = ket::Profile::current().monoFamily(); !family.isEmpty())
        mono.setFamily(family);
    mono.setStyleHint(QFont::Monospace);
    mono.setPixelSize(13); // the mock's 13 px
    ssh.fingerprint->setFont(mono);
    keyLayout->addWidget(ssh.fingerprint);
    auto* decision = new QHBoxLayout;
    decision->setSpacing(8);
    ssh.reject = new ket::Button(tr("Reject"), ket::ButtonVariant::Ghost, ssh.hostKey);
    ssh.reject->setObjectName(QStringLiteral("linuxSshRejectKey"));
    ssh.accept =
        new ket::Button(tr("Accept and connect"), ket::ButtonVariant::Primary, ssh.hostKey);
    ssh.accept->setObjectName(QStringLiteral("linuxSshAcceptKey"));
    decision->addStretch(1);
    decision->addWidget(ssh.reject);
    decision->addWidget(ssh.accept);
    keyLayout->addLayout(decision);
    ssh.hostKey->hide();
    layout->addWidget(ssh.hostKey);
    return ssh;
}

// The Relay page: the pairing link, its feedback and the identity check.
struct RelayPage final {
    QWidget* page;
    QWidget* pairing;
    ket::TextField* link;
    QLabel* feedback;
    QCheckBox* match;
    QLabel* gate;
    ket::Button* signIn;
};

RelayPage makeRelayPage(QWidget* parent) {
    RelayPage relay{};
    relay.page = new QWidget(parent);
    relay.page->setObjectName(QStringLiteral("linuxRelayPage"));
    auto* layout = new QVBoxLayout(relay.page);
    layout->setContentsMargins(QMargins());
    layout->setSpacing(12);
    relay.gate = new QLabel(relay.page);
    relay.gate->setObjectName(QStringLiteral("linuxRelayGate"));
    relay.gate->setWordWrap(true);
    relay.gate->hide();
    layout->addWidget(relay.gate);
    relay.signIn = new ket::Button(tr("Sign in"), ket::ButtonVariant::Secondary, relay.page);
    relay.signIn->setObjectName(QStringLiteral("linuxRelaySignIn"));
    relay.signIn->hide();
    layout->addWidget(relay.signIn, 0, Qt::AlignLeft);
    relay.pairing = new QWidget(relay.page);
    auto* pairing = new QVBoxLayout(relay.pairing);
    pairing->setContentsMargins(QMargins());
    relay.link = new ket::TextField(relay.pairing);
    relay.link->setObjectName(QStringLiteral("linuxPairingLink"));
    relay.link->setAccessibleName(tr("One-time Linux pairing link"));
    // A one-time secret, not a password: it is masked while it sits in the field.
    relay.link->setEchoMode(QLineEdit::Password);
    relay.link->setPlaceholderText(QStringLiteral("ketplus://pair?ticket=…&secret=…"));
    // The mock's field row: the label beside the field, the instruction as its help.
    auto* row = new ket::FieldRow(tr("Pairing link"), relay.link,
                                  tr("Run ketplusctl pairing create on the machine and paste "
                                     "the ketplus:// link it shows."),
                                  relay.pairing);
    row->setObjectName(QStringLiteral("linuxPairingRow"));
    pairing->addWidget(row);
    relay.feedback = feedbackLabel("linuxPairingFeedback", relay.pairing);
    pairing->addWidget(relay.feedback);
    relay.match = new QCheckBox(tr("I checked the identity on both machines."), relay.pairing);
    relay.match->hide();
    pairing->addWidget(relay.match);
    layout->addWidget(relay.pairing);
    return relay;
}

} // namespace

ket::Dialog* openLinuxConnectDialog(QWidget* anchor, LinuxMachineModel* model,
                                    remote::ControlV2Client* client, LinuxConnectHooks hooks,
                                    const LinuxConnectMode initialMode, const QString& sshHost,
                                    bool manageDaemon) {
    // An in-window card like the other dialogs (#422); it goes once it finishes.
    auto* dialog = new ket::Dialog(tr("Connect a Linux machine"), anchor);
    dialog->setObjectName(QStringLiteral("linuxPairingDialog"));
    dialog->setCardWidth(500);
    QObject::connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(QMargins());
    layout->setSpacing(16);
    auto* modes = new ket::SegmentedControl({tr("SSH (free)"), tr("ketPlus Relay (Pro)")}, body);
    modes->setObjectName(QStringLiteral("linuxConnectMode"));
    layout->addWidget(modes, 0, Qt::AlignLeft);
    // Without an attached SSH backend nothing would answer Connect, so only the Relay is offered.
    const bool sshAvailable = model && model->backendsAttached();
    if (!sshAvailable)
        modes->hide();
    const SshPage ssh = makeSshPage(body);
    layout->addWidget(ssh.page);
    const RelayPage relay = makeRelayPage(body);
    layout->addWidget(relay.page);
    dialog->setBody(body);

    auto* cancel = dialog->addButton(tr("Cancel"), ket::ButtonVariant::Ghost);
    auto* sshConnect = dialog->addButton(tr("Connect"), ket::ButtonVariant::Primary);
    sshConnect->setObjectName(QStringLiteral("linuxSshConnect"));
    auto* check = dialog->addButton(tr("Check link"));
    auto* approve = dialog->addButton(tr("Check approval"), ket::ButtonVariant::Primary);
    approve->setEnabled(false);

    const QPointer<ket::Dialog> dialogGuard(dialog);
    const QPointer<LinuxMachineModel> modelGuard(model);
    const QPointer<remote::ControlV2Client> clientGuard(client);
    auto shared = std::make_shared<LinuxConnectHooks>(std::move(hooks));
    // The host whose connection is in progress; empty when none is.
    auto pendingHost = std::make_shared<QString>();
    auto ticketId = std::make_shared<QString>();

    const auto applyRelayGate = [relay, check, approve, modelGuard, clientGuard] {
        // Signed out, the account cannot be known yet: sign in first, then the plan decides.
        const bool signedIn = clientGuard && clientGuard->signedIn();
        const bool entitled = signedIn && modelGuard && modelGuard->relayEntitled();
        relay.gate->setVisible(!entitled);
        relay.gate->setText(
            signedIn ? tr("Pairing through the ketPlus Relay is part of ketPlus Pro. Use SSH, "
                          "or upgrade your KetSuite account to ketPlus Pro to pair through the "
                          "Relay.")
                     : tr("Sign in with the KetSuite account the Linux daemon uses to pair "
                          "through the ketPlus Relay."));
        relay.signIn->setVisible(!signedIn);
        relay.pairing->setVisible(entitled);
        check->setEnabled(entitled);
        if (!entitled)
            approve->setEnabled(false);
    };
    const auto showMode = [=](const int index) {
        const bool isSsh = index == 0;
        ssh.page->setVisible(isSsh);
        relay.page->setVisible(!isSsh);
        sshConnect->setVisible(isSsh);
        check->setVisible(!isSsh);
        approve->setVisible(!isSsh);
        if (!isSsh)
            applyRelayGate();
    };
    QObject::connect(modes, &ket::SegmentedControl::currentChanged, dialog, showMode);
    if (model)
        QObject::connect(model, &LinuxMachineModel::relayEntitledChanged, dialog, applyRelayGate);
    if (client)
        QObject::connect(client, &remote::ControlV2Client::signInFinished, dialog, applyRelayGate);
    QObject::connect(relay.signIn, &QAbstractButton::clicked, dialog, [shared] {
        if (shared->signIn)
            shared->signIn();
    });

    // SSH: Connect, then maybe a host key, then connected or a failure.
    static_cast<QVBoxLayout*>(ssh.page->layout())
        ->addWidget(makeLinuxDaemonInstallCard(model, pendingHost, ssh.page));
    const auto resetSsh = [ssh, sshConnect, pendingHost] {
        pendingHost->clear();
        ssh.hostKey->hide();
        ssh.host->setEnabled(true);
        sshConnect->setEnabled(true);
        ssh.host->setFocus();
    };
    // Enter in the host field connects, as the dialog's primary action.
    QObject::connect(ssh.host, &QLineEdit::returnPressed, sshConnect, [sshConnect] {
        if (sshConnect->isVisible() && sshConnect->isEnabled())
            sshConnect->click();
    });
    QObject::connect(sshConnect, &QAbstractButton::clicked, dialog,
                     [ssh, sshConnect, cancel, pendingHost, modelGuard, manageDaemon] {
                         const QString host = ssh.host->text().trimmed();
                         if (const QString problem = validateSshHost(host); !problem.isEmpty()) {
                             showFeedback(ssh.feedback, problem, true);
                             ssh.host->setFocus();
                             return;
                         }
                         *pendingHost = host;
                         ssh.host->setEnabled(false);
                         sshConnect->setEnabled(false);
                         // Keep the keyboard in the card: Cancel ends the attempt.
                         cancel->setFocus();
                         showFeedback(ssh.feedback, tr("Connecting to %1…").arg(host));
                         if (modelGuard) {
                             if (manageDaemon)
                                 modelGuard->requestDaemonManagement(host);
                             else
                                 modelGuard->requestSshConnect(host);
                         }
                     });
    if (model) {
        QObject::connect(
            model, &LinuxMachineModel::hostKeyPresented, dialog,
            [ssh, pendingHost](const QString& host, const QString& fingerprint) {
                if (host != *pendingHost)
                    return;
                ssh.hostKeyPrompt->setText(
                    tr("This is the first connection to %1. Compare its host key with the one "
                       "on the machine (ssh-keygen -lf on its public host key) before you "
                       "accept it.")
                        .arg(host));
                ssh.fingerprint->setText(fingerprint);
                ssh.fingerprint->setProperty("fingerprint", fingerprint);
                showFeedback(ssh.feedback, {});
                ssh.hostKey->show();
                // Focus the safe choice: Enter must never trust a key unread.
                ssh.reject->setFocus();
            });
        QObject::connect(
            model, &LinuxMachineModel::sshFailed, dialog,
            [ssh, pendingHost, resetSsh](const QString& host, const LinuxMachine::Error error,
                                         const QString& text) {
                if (host != *pendingHost)
                    return;
                resetSsh();
                showFeedback(ssh.feedback,
                             LinuxMachineModel::errorMessage(
                                 error, host, text, LinuxMachineModel::knownHostsName(host)),
                             true);
            });
        QObject::connect(model, &LinuxMachineModel::sshConnected, dialog,
                         [dialog, pendingHost, shared](const QString& host) {
                             if (host != *pendingHost)
                                 return;
                             pendingHost->clear();
                             if (shared->finished)
                                 shared->finished(tr("%1 connected over SSH.").arg(host));
                             dialog->accept();
                         });
        QObject::connect(
            model, &LinuxMachineModel::daemonOperationFinished, dialog,
            [dialog, pendingHost, shared](const QString& host, const QString& message) {
                if (host != *pendingHost)
                    return;
                pendingHost->clear();
                if (shared->finished)
                    shared->finished(message);
                dialog->accept();
            });
    }
    const auto decide = [ssh, pendingHost, modelGuard, resetSsh](const bool accepted) {
        const QString host = *pendingHost;
        const QString fingerprint = ssh.fingerprint->property("fingerprint").toString();
        if (host.isEmpty())
            return;
        if (modelGuard)
            modelGuard->decideHostKey(host, fingerprint, accepted);
        if (accepted) {
            ssh.hostKey->hide();
            showFeedback(ssh.feedback, tr("Connecting to %1…").arg(host));
            return;
        }
        resetSsh();
        showFeedback(ssh.feedback,
                     tr("Host key rejected. ketPlus did not connect to %1.").arg(host), true);
    };
    QObject::connect(ssh.accept, &QAbstractButton::clicked, dialog, [decide] { decide(true); });
    QObject::connect(ssh.reject, &QAbstractButton::clicked, dialog, [decide] { decide(false); });

    // Closing the card ends whatever it started.
    QObject::connect(dialog, &ket::Dialog::finished, dialog,
                     [pendingHost, modelGuard, clientGuard](const bool accepted) {
                         if (!accepted && !pendingHost->isEmpty() && modelGuard)
                             modelGuard->cancelSshConnect(*pendingHost);
                         if (clientGuard)
                             clientGuard->cancelPairing();
                     });
    if (client)
        QObject::connect(dialog, &QObject::destroyed, client, [clientGuard] {
            if (clientGuard)
                clientGuard->cancelPairing();
        });
    QObject::connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);

    // Relay: the existing pairing flow (#88): check the link, compare the digest, approve.
    QObject::connect(relay.match, &QCheckBox::toggled, approve, &QAbstractButton::setEnabled);
    QObject::connect(
        check, &QAbstractButton::clicked, dialog,
        [relay, check, ticketId, dialogGuard, clientGuard] {
            const QUrl link(relay.link->text().trimmed());
            relay.link->clear();
            const QUrlQuery query(link);
            const auto ticket = query.queryItemValue(QStringLiteral("ticket"));
            const auto secret = query.queryItemValue(QStringLiteral("secret"));
            if (link.scheme() != QStringLiteral("ketplus") ||
                link.host() != QStringLiteral("pair") || ticket.isEmpty() || secret.size() < 32) {
                showFeedback(relay.feedback,
                             tr("Paste the complete, unexpired pairing link from Linux."));
                relay.link->setFocus();
                return;
            }
            if (!clientGuard)
                return;
            check->setEnabled(false);
            showFeedback(relay.feedback, tr("Checking the Linux pairing request…"));
            // The link names the Linux host's keys; the client pins them (#88).
            const QJsonObject host{
                {QStringLiteral("machineId"),
                 query.queryItemValue(QStringLiteral("machine"), QUrl::FullyDecoded)},
                {QStringLiteral("keyVersion"), query.queryItemValue(QStringLiteral("kv")).toInt()},
                {QStringLiteral("fingerprint"), query.queryItemValue(QStringLiteral("fp"))}};
            clientGuard->claimPairing(
                ticket, secret, QStringLiteral("KetPlus Desktop"), host,
                [relay, check, ticketId, ticket, dialogGuard](const QJsonValue& value,
                                                              const QString& error) {
                    if (!dialogGuard)
                        return;
                    check->setEnabled(true);
                    if (!error.isEmpty()) {
                        showFeedback(relay.feedback, error);
                        return;
                    }
                    *ticketId = ticket;
                    const auto digest =
                        value.toObject().value(QStringLiteral("claimDigest")).toString();
                    showFeedback(relay.feedback,
                                 tr("Compare the first six characters of claimDigest "
                                    "shown by ketplusctl pairing status: %1")
                                     .arg(digest.left(6).toUpper()));
                    relay.match->show();
                });
        });
    QObject::connect(
        approve, &QAbstractButton::clicked, dialog,
        [relay, approve, ticketId, dialogGuard, clientGuard, shared] {
            if (!clientGuard)
                return;
            approve->setEnabled(false);
            clientGuard->pairingStatus(
                *ticketId, [relay, approve, dialogGuard, clientGuard,
                            shared](const QJsonValue& value, const QString& error) {
                    if (!dialogGuard || !clientGuard)
                        return;
                    if (!error.isEmpty()) {
                        showFeedback(relay.feedback, error);
                        approve->setEnabled(true);
                        return;
                    }
                    const auto ticket = value.toObject();
                    if (ticket.value(QStringLiteral("status")) != QStringLiteral("approved")) {
                        showFeedback(relay.feedback,
                                     tr("Approve the request on Linux, then check again."));
                        approve->setEnabled(true);
                        return;
                    }
                    QString failure;
                    if (!clientGuard->acceptPairing(ticket, &failure)) {
                        showFeedback(relay.feedback, failure);
                        approve->setEnabled(true);
                        return;
                    }
                    if (shared->finished)
                        shared->finished(tr("Linux machine trusted."));
                    dialogGuard->accept();
                });
        });

    const int index = initialMode == LinuxConnectMode::Ssh && sshAvailable ? 0 : 1;
    modes->setCurrentIndex(index);
    showMode(index);
    dialog->open();
    (index == 0 ? static_cast<QWidget*>(ssh.host) : relay.link)->setFocus();
    if (index == 0 && !sshHost.isEmpty()) {
        ssh.host->setText(sshHost);
        sshConnect->click();
    }
    return dialog;
}

} // namespace ketplus
