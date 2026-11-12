#include "remote/LinuxMachineRow.h"

#include "KetAppearance.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetIcons.h"
#include "KetProfile.h"
#include "KetTokens.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("LinuxMachineRow", text); }

bool attached(const LinuxMachineModel* model) { return model && model->backendsAttached(); }

// Connected over the Relay means the Relay answers; the host itself may be away.
bool hostAway(const LinuxMachine& machine) {
    return LinuxMachineModel::hostAway(machine, QDateTime::currentDateTimeUtc());
}

// What the route cannot do right now, for the row and Details: the Relay without ketPlus Pro.
QString unavailableText(const LinuxMachine& machine) {
    return machine.connectable ? QString()
                               : tr("The ketPlus Relay is part of ketPlus Pro. Upgrade your "
                                    "KetSuite account to use this route; SSH stays free.");
}

// Requests over the Relay wait for the host (ADR 0119: up to 15 minutes), so nothing may suggest
// they run at once while the link or the host is not there.
QString waitText(const LinuxMachine& machine) {
    if (machine.route != LinuxMachine::Route::Relay || !machine.connectable)
        return {};
    if (hostAway(machine))
        return tr("Requests wait until the host is back, for up to 15 minutes.");
    if (machine.state == LinuxMachine::State::Connecting ||
        machine.state == LinuxMachine::State::Reconnecting)
        return tr("Requests made now wait for the Relay, for up to 15 minutes.");
    return {};
}

QString pendingText(const LinuxMachine& machine) {
    if (machine.pendingCommands <= 0)
        return {};
    return machine.pendingCommands == 1
               ? tr("1 request sent, outcome pending")
               : tr("%1 requests sent, outcome pending").arg(machine.pendingCommands);
}

QString badgeTone(const LinuxMachine& machine, const bool backendsAttached) {
    if (machine.error != LinuxMachine::Error::None)
        return QStringLiteral("danger");
    if (!backendsAttached && machine.route == LinuxMachine::Route::Relay)
        return QStringLiteral("positive");
    if (!machine.connectable)
        return {};
    if (machine.state == LinuxMachine::State::Connected && hostAway(machine))
        return QStringLiteral("warning");
    switch (machine.state) {
    case LinuxMachine::State::Connected:
        return QStringLiteral("positive");
    case LinuxMachine::State::Connecting:
    case LinuxMachine::State::Reconnecting:
        return QStringLiteral("warning");
    case LinuxMachine::State::Offline:
        break;
    }
    return {};
}

// Offline machines offer Connect; any other state can be ended with Disconnect. A route that
// cannot be used offers neither.
bool offersConnect(const LinuxMachine& machine) {
    return machine.connectable && machine.state == LinuxMachine::State::Offline;
}
bool offersDisconnect(const LinuxMachine& machine) {
    return machine.connectable && machine.state != LinuxMachine::State::Offline;
}

QLabel* roleLabel(const QString& text, const char* role, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("remoteRole", QString::fromLatin1(role));
    return label;
}

QLabel* badge(const QString& text, const QString& tone, QWidget* parent) {
    auto* label = roleLabel(text, "badge", parent);
    if (!tone.isEmpty())
        label->setProperty("remoteTone", tone);
    return label;
}

QString errorText(const LinuxMachine& machine) { return LinuxMachineModel::errorMessage(machine); }

} // namespace

QString linuxMachineBadgeText(const LinuxMachine& machine, const bool backendsAttached) {
    switch (machine.error) {
    case LinuxMachine::Error::None:
        if (!backendsAttached && machine.route == LinuxMachine::Route::Relay)
            return tr("Available");
        if (!machine.connectable)
            return tr("Needs Pro");
        if (machine.state == LinuxMachine::State::Connected && hostAway(machine))
            return tr("Host offline");
        return LinuxMachineModel::stateText(machine.state);
    case LinuxMachine::Error::HostKeyMismatch:
        return tr("Host key changed");
    case LinuxMachine::Error::AuthenticationFailed:
        return tr("SSH key refused");
    case LinuxMachine::Error::DaemonNotInstalled:
        return tr("Daemon not installed");
    case LinuxMachine::Error::IdentityChanged:
        return tr("Identity changed");
    case LinuxMachine::Error::Other:
        break;
    }
    return tr("Cannot connect");
}

QFrame* makeLinuxMachineRow(const LinuxMachine& machine, LinuxMachineModel* model,
                            QWidget* parent) {
    const auto theme = ket::Appearance::instance().theme();
    auto* row = new QFrame(parent);
    row->setObjectName(QStringLiteral("linuxMachineRow"));
    row->setProperty("remoteRole", QStringLiteral("deviceRow"));
    row->setProperty("machineId", machine.id);
    row->setMinimumHeight(94);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(18);
    auto* iconSurface = new QFrame(row);
    iconSurface->setProperty("remoteRole", QStringLiteral("deviceIconSurface"));
    iconSurface->setFixedSize(40, 40);
    auto* iconLayout = new QVBoxLayout(iconSurface);
    iconLayout->setContentsMargins(10, 10, 10, 10);
    auto* icon = new QLabel(iconSurface);
    icon->setPixmap(ket::Icons::pixmap(ket::Icons::codeForLucide(u"terminal"), 18,
                                       row->devicePixelRatioF(),
                                       ket::Tokens::color(u"text-secondary", theme)));
    iconLayout->addWidget(icon, 0, Qt::AlignCenter);
    layout->addWidget(iconSurface, 0, Qt::AlignVCenter);

    auto* copy = new QVBoxLayout;
    copy->setSpacing(6);
    copy->addStretch(1);
    // The name with its state and route, as the mock's badges.
    auto* nameLine = new QHBoxLayout;
    nameLine->setSpacing(8);
    auto* name = roleLabel(machine.name, "deviceName", row);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    nameLine->addWidget(name);
    const bool live = attached(model);
    auto* state = badge(linuxMachineBadgeText(machine, live), badgeTone(machine, live), row);
    state->setObjectName(QStringLiteral("linuxMachineState"));
    state->setAccessibleName(tr("Machine %1: %2").arg(machine.name, state->text()));
    nameLine->addWidget(state);
    auto* route = badge(LinuxMachineModel::routeText(machine.route), {}, row);
    route->setObjectName(QStringLiteral("linuxMachineRoute"));
    nameLine->addWidget(route);
    nameLine->addStretch(1);
    copy->addLayout(nameLine);
    auto* detail = roleLabel(machine.detail, "deviceMeta", row);
    detail->setObjectName(QStringLiteral("linuxMachineDetail"));
    copy->addWidget(detail);
    // The Relay host's presence once it is away, what waits for it, and why the route is closed.
    QStringList notes;
    if (hostAway(machine))
        notes.append(LinuxMachineModel::presenceText(machine, QDateTime::currentDateTimeUtc()));
    if (const auto pending = pendingText(machine); !pending.isEmpty())
        notes.append(pending);
    if (const auto closed = unavailableText(machine); !closed.isEmpty())
        notes.append(closed);
    if (!notes.isEmpty()) {
        auto* note = roleLabel(notes.join(QStringLiteral(" · ")), "deviceMeta", row);
        note->setObjectName(QStringLiteral("linuxMachinePresence"));
        note->setWordWrap(true);
        if (hostAway(machine))
            note->setProperty("remoteTone", QStringLiteral("warning"));
        copy->addWidget(note);
    }
    if (machine.error != LinuxMachine::Error::None) {
        auto* error = new QLabel(errorText(machine), row);
        error->setObjectName(QStringLiteral("linuxMachineError"));
        error->setProperty("remoteTone", QStringLiteral("danger"));
        error->setWordWrap(true);
        error->setTextInteractionFlags(Qt::TextSelectableByMouse);
        copy->addWidget(error);
    }
    // The Relay stands in for SSH: say why SSH is not used, and offer it again.
    if (!machine.sshFailure.isEmpty()) {
        // The Relay stands in for SSH; it is "in use" only while it is up.
        const bool relayUp = machine.state != LinuxMachine::State::Offline;
        auto* failure = new QLabel((relayUp ? tr("Using the Relay because SSH failed. %1")
                                            : tr("SSH failed, so Connect uses the Relay. %1"))
                                       .arg(machine.sshFailure),
                                   row);
        failure->setObjectName(QStringLiteral("linuxMachineSshFailure"));
        failure->setProperty("remoteTone", QStringLiteral("warning"));
        failure->setWordWrap(true);
        failure->setTextInteractionFlags(Qt::TextSelectableByMouse);
        copy->addWidget(failure);
    }
    copy->addStretch(1);
    layout->addLayout(copy, 1);

    auto* details = new ket::Button(tr("Details"), ket::ButtonVariant::Ghost, row);
    details->setObjectName(QStringLiteral("linuxMachineDetails"));
    details->setProperty("remoteTertiary", true);
    const QPointer<LinuxMachineModel> guard(model);
    const QString id = machine.id;
    QObject::connect(details, &QAbstractButton::clicked, row, [guard, id, row] {
        if (guard)
            showLinuxMachineDetails(id, guard, row);
    });
    layout->addWidget(details, 0, Qt::AlignVCenter);
    if (live && !machine.sshFailure.isEmpty()) {
        auto* useSsh = new ket::Button(tr("Use SSH"), ket::ButtonVariant::Ghost, row);
        useSsh->setObjectName(QStringLiteral("linuxMachineUseSsh"));
        QObject::connect(useSsh, &QAbstractButton::clicked, row, [guard, id] {
            if (guard)
                guard->requestSshRoute(id);
        });
        layout->addWidget(useSsh, 0, Qt::AlignVCenter);
    }
    // Nothing would carry out Connect or Disconnect before the backends are attached.
    if (!live || (!offersConnect(machine) && !offersDisconnect(machine)))
        return row;
    const bool connect = offersConnect(machine);
    auto* toggle =
        new ket::Button(connect ? tr("Connect") : tr("Disconnect"), ket::ButtonVariant::Ghost, row);
    toggle->setObjectName(QStringLiteral("linuxMachineToggle"));
    if (const auto wait = waitText(machine); connect && !wait.isEmpty())
        toggle->setToolTip(wait);
    QObject::connect(toggle, &QAbstractButton::clicked, row, [guard, id, connect] {
        if (!guard)
            return;
        connect ? guard->requestConnect(id) : guard->requestDisconnect(id);
    });
    layout->addWidget(toggle, 0, Qt::AlignVCenter);
    return row;
}

void showLinuxMachineDetails(const QString& id, LinuxMachineModel* model, QWidget* anchor) {
    const auto* initial = model ? model->machine(id) : nullptr;
    if (!initial)
        return;
    const LinuxMachine machine = *initial;
    auto* dialog = new ket::Dialog(machine.name, anchor);
    dialog->setObjectName(QStringLiteral("linuxMachineDetailsDialog"));
    // Wider than the mock's 500 px: the whole SHA256 key must show to be compared.
    dialog->setCardWidth(580);
    QObject::connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    auto* body = new QWidget;
    auto* form = new QFormLayout(body);
    form->setContentsMargins(QMargins());
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(10);
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto value = [body](const char* objectName) {
        auto* label = new QLabel(body);
        label->setObjectName(QString::fromLatin1(objectName));
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    };
    auto* route = value("linuxDetailsRoute");
    route->setText(machine.route == LinuxMachine::Route::Ssh
                       ? tr("SSH, with your SSH keys and known_hosts")
                       : tr("ketPlus Relay, end-to-end encrypted"));
    form->addRow(tr("Route"), route);
    auto* state = value("linuxDetailsState");
    form->addRow(tr("State"), state);
    // The Relay host's own presence: Connected means only that the Relay answers.
    auto* host = value("linuxDetailsHost");
    host->setWordWrap(false);
    form->addRow(tr("Host"), host);
    // What waits for the host: pending outcomes, and how long new requests may wait.
    auto* wait = value("linuxDetailsWait");
    // Short lines, laid out unwrapped: a wrapped label in the form is sized a line too tall.
    wait->setWordWrap(false);
    form->addRow(QString(), wait);
    auto* fingerprint = value("linuxDetailsFingerprint");
    const QFont plain = fingerprint->font();
    QFont mono = plain;
    if (const QString family = ket::Profile::current().monoFamily(); !family.isEmpty())
        mono.setFamily(family);
    mono.setStyleHint(QFont::Monospace);
    mono.setPixelSize(13); // the mock's 13 px, so a SHA256 key fits the card on one line
    // Over SSH, the SSH server's host key; over the Relay, the daemon identity pinned at pairing.
    // They are different keys, so they are named apart.
    form->addRow(machine.route == LinuxMachine::Route::Ssh ? tr("Host key") : tr("Host identity"),
                 fingerprint);
    auto* verifiedLabel = value("linuxDetailsVerified");
    form->addRow(QString(), verifiedLabel);
    // ADR 0119 "Host key rotation": both daemon identities are shown before the user trusts one.
    auto* trustedIdentity = value("linuxDetailsTrustedIdentity");
    form->addRow(tr("Trusted identity"), trustedIdentity);
    auto* presentedIdentity = value("linuxDetailsPresentedIdentity");
    form->addRow(tr("Presented identity"), presentedIdentity);
    auto* unavailable = value("linuxDetailsUnavailable");
    form->addRow(unavailable);
    auto* error = value("linuxDetailsError");
    error->setProperty("remoteTone", QStringLiteral("danger"));
    form->addRow(error);
    dialog->setBody(body);
    auto* close = dialog->addButton(tr("Close"), ket::ButtonVariant::Ghost);
    QObject::connect(close, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    const QPointer<LinuxMachineModel> guard(model);
    ket::Button* disconnect = nullptr;
    ket::Button* connect = nullptr;
    ket::Button* forget = nullptr;
    ket::Button* trust = nullptr;
    // Nothing would carry out these before the backends are attached.
    if (model->backendsAttached()) {
        // Only an SSH machine is ketPlus's own to remove; a Relay pairing is ended on the host.
        if (machine.route == LinuxMachine::Route::Ssh || !machine.sshTarget.isEmpty()) {
            auto* remove = dialog->addButton(tr("Remove"), ket::ButtonVariant::Ghost);
            remove->setObjectName(QStringLiteral("linuxDetailsRemove"));
            QObject::connect(remove, &QAbstractButton::clicked, dialog, [guard, id, dialog] {
                if (guard)
                    guard->requestRemove(id);
                dialog->accept();
            });
        }
        // The re-trust paths: shown only while their failure is the machine's.
        forget = dialog->addButton(tr("Forget old key"));
        forget->setObjectName(QStringLiteral("linuxDetailsForgetKey"));
        QObject::connect(forget, &QAbstractButton::clicked, dialog, [guard, id, dialog] {
            if (guard)
                guard->requestForgetHostKey(id);
            dialog->accept();
        });
        trust = dialog->addButton(tr("Trust new identity"));
        trust->setObjectName(QStringLiteral("linuxDetailsTrustIdentity"));
        QObject::connect(trust, &QAbstractButton::clicked, dialog, [guard, id, dialog] {
            if (guard)
                guard->requestTrustIdentity(id);
            dialog->accept();
        });
        disconnect = dialog->addButton(tr("Disconnect"));
        disconnect->setObjectName(QStringLiteral("linuxDetailsDisconnect"));
        QObject::connect(disconnect, &QAbstractButton::clicked, dialog, [guard, id, dialog] {
            if (guard)
                guard->requestDisconnect(id);
            dialog->accept();
        });
        connect = dialog->addButton(tr("Connect"), ket::ButtonVariant::Primary);
        connect->setObjectName(QStringLiteral("linuxDetailsConnect"));
        QObject::connect(connect, &QAbstractButton::clicked, dialog, [guard, id, dialog] {
            if (guard)
                guard->requestConnect(id);
            dialog->accept();
        });
    }
    // What can change while the card is open: the state, the key once verified, the error.
    const auto refresh = [guard, id, dialog, form, state, host, wait, unavailable, fingerprint,
                          verifiedLabel, trustedIdentity, presentedIdentity, error, connect,
                          disconnect, forget, trust, plain, mono] {
        const auto* current = guard ? guard->machine(id) : nullptr;
        if (!current) {
            dialog->reject();
            return;
        }
        state->setText(linuxMachineBadgeText(*current, guard->backendsAttached()));
        const auto presence =
            LinuxMachineModel::presenceText(*current, QDateTime::currentDateTimeUtc());
        host->setText(presence);
        host->setProperty("remoteTone", hostAway(*current) ? QStringLiteral("warning") : QString());
        form->setRowVisible(host, !presence.isEmpty());
        QStringList waits;
        for (const auto& line : {pendingText(*current), waitText(*current)})
            if (!line.isEmpty())
                waits.append(line);
        wait->setText(waits.join(QLatin1Char('\n')));
        form->setRowVisible(wait, !wait->text().isEmpty());
        unavailable->setText(unavailableText(*current));
        form->setRowVisible(unavailable, !unavailable->text().isEmpty());
        const bool verified = !current->fingerprint.isEmpty();
        // A key that did not verify is shown as what it is, never as the machine's key.
        const bool offered = !verified && !current->presentedFingerprint.isEmpty();
        const QString key = verified ? current->fingerprint : current->presentedFingerprint;
        const bool known = verified || offered;
        fingerprint->setText(known ? key : tr("Not verified yet"));
        fingerprint->setFont(known ? mono : plain);
        // A key has no spaces to wrap at: let it shrink rather than push the label column.
        fingerprint->setWordWrap(!known);
        fingerprint->setSizePolicy(known ? QSizePolicy::Ignored : QSizePolicy::Preferred,
                                   QSizePolicy::Preferred);
        fingerprint->setToolTip(known ? key : QString());
        verifiedLabel->setText(verified ? tr("Verified")
                                        : tr("Does not match the key accepted before"));
        verifiedLabel->setProperty("remoteTone", offered ? QStringLiteral("danger") : QString());
        form->setRowVisible(verifiedLabel, known);
        const bool failed = current->error != LinuxMachine::Error::None;
        if (failed)
            error->setText(errorText(*current));
        form->setRowVisible(error, failed);
        if (connect)
            connect->setEnabled(offersConnect(*current));
        if (disconnect)
            disconnect->setEnabled(offersDisconnect(*current));
        if (forget)
            forget->setVisible(current->canForgetHostKey);
        if (trust)
            trust->setVisible(current->error == LinuxMachine::Error::IdentityChanged);
        const bool rotated = current->error == LinuxMachine::Error::IdentityChanged;
        trustedIdentity->setText(current->id);
        trustedIdentity->setFont(mono);
        presentedIdentity->setText(current->presentedIdentity.isEmpty()
                                       ? tr("None: the daemon named no identity")
                                       : current->presentedIdentity);
        presentedIdentity->setFont(current->presentedIdentity.isEmpty() ? plain : mono);
        presentedIdentity->setProperty("remoteTone", QStringLiteral("danger"));
        form->setRowVisible(trustedIdentity, rotated);
        form->setRowVisible(presentedIdentity, rotated);
        // Nothing to trust when the daemon named no identity.
        if (trust)
            trust->setEnabled(rotated && !current->presentedIdentity.isEmpty());
    };
    refresh();
    QObject::connect(model, &LinuxMachineModel::machinesChanged, dialog, refresh);
    dialog->open();
}

} // namespace ketplus
