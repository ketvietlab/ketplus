#include "remote/LinuxMachineRow.h"

#include "KetAppearance.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetIcons.h"
#include "KetProfile.h"
#include "KetTokens.h"

#include <QCoreApplication>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("LinuxMachineRow", text); }

QString badgeTone(const LinuxMachine& machine) {
    if (machine.error != LinuxMachine::Error::None)
        return QStringLiteral("danger");
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

// Offline machines offer Connect; any other state can be ended with Disconnect.
bool offersConnect(const LinuxMachine& machine) {
    return machine.state == LinuxMachine::State::Offline;
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

QString errorText(const LinuxMachine& machine) {
    return LinuxMachineModel::errorMessage(machine.error, machine.name, machine.errorText);
}

} // namespace

QString linuxMachineBadgeText(const LinuxMachine& machine) {
    switch (machine.error) {
    case LinuxMachine::Error::None:
        return LinuxMachineModel::stateText(machine.state);
    case LinuxMachine::Error::HostKeyMismatch:
        return tr("Host key changed");
    case LinuxMachine::Error::AuthenticationFailed:
        return tr("SSH key refused");
    case LinuxMachine::Error::DaemonNotInstalled:
        return tr("Daemon not installed");
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
    auto* state = badge(linuxMachineBadgeText(machine), badgeTone(machine), row);
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
    if (machine.error != LinuxMachine::Error::None) {
        auto* error = new QLabel(errorText(machine), row);
        error->setObjectName(QStringLiteral("linuxMachineError"));
        error->setProperty("remoteTone", QStringLiteral("danger"));
        error->setWordWrap(true);
        error->setTextInteractionFlags(Qt::TextSelectableByMouse);
        copy->addWidget(error);
    }
    copy->addStretch(1);
    layout->addLayout(copy, 1);

    auto* details = new ket::Button(tr("Details"), ket::ButtonVariant::Ghost, row);
    details->setObjectName(QStringLiteral("linuxMachineDetails"));
    details->setProperty("remoteTertiary", true);
    const QPointer<LinuxMachineModel> guard(model);
    const QString id = machine.id;
    QObject::connect(details, &QAbstractButton::clicked, row, [guard, id, row] {
        if (!guard)
            return;
        if (const auto* current = guard->machine(id))
            showLinuxMachineDetails(*current, guard, row);
    });
    layout->addWidget(details, 0, Qt::AlignVCenter);
    const bool connect = offersConnect(machine);
    auto* toggle =
        new ket::Button(connect ? tr("Connect") : tr("Disconnect"), ket::ButtonVariant::Ghost, row);
    toggle->setObjectName(QStringLiteral("linuxMachineToggle"));
    QObject::connect(toggle, &QAbstractButton::clicked, row, [guard, id, connect] {
        if (!guard)
            return;
        connect ? guard->requestConnect(id) : guard->requestDisconnect(id);
    });
    layout->addWidget(toggle, 0, Qt::AlignVCenter);
    return row;
}

void showLinuxMachineDetails(const LinuxMachine& machine, LinuxMachineModel* model,
                             QWidget* anchor) {
    auto* dialog = new ket::Dialog(machine.name, anchor);
    dialog->setObjectName(QStringLiteral("linuxMachineDetailsDialog"));
    // Wider than the mock's 500 px: the whole SHA256 key must show to be compared.
    dialog->setCardWidth(540);
    QObject::connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    auto* body = new QWidget;
    auto* form = new QFormLayout(body);
    form->setContentsMargins(QMargins());
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(10);
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto value = [body](const QString& text, const char* objectName) {
        auto* label = new QLabel(text, body);
        label->setObjectName(QString::fromLatin1(objectName));
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    };
    form->addRow(tr("Route"), value(machine.route == LinuxMachine::Route::Ssh
                                        ? tr("SSH, with your SSH keys and known_hosts")
                                        : tr("ketPlus Relay, end-to-end encrypted"),
                                    "linuxDetailsRoute"));
    form->addRow(tr("State"), value(linuxMachineBadgeText(machine), "linuxDetailsState"));
    const bool verified = !machine.fingerprint.isEmpty();
    auto* fingerprint =
        value(verified ? machine.fingerprint : tr("Not verified yet"), "linuxDetailsFingerprint");
    if (verified) {
        QFont mono = fingerprint->font();
        if (const QString family = ket::Profile::current().monoFamily(); !family.isEmpty())
            mono.setFamily(family);
        mono.setStyleHint(QFont::Monospace);
        mono.setPixelSize(13); // the mock's 13 px, so a SHA256 key fits the card on one line
        fingerprint->setFont(mono);
        // A key has no spaces to wrap at: let it shrink rather than push the label column.
        fingerprint->setWordWrap(false);
        fingerprint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        fingerprint->setToolTip(machine.fingerprint);
    }
    form->addRow(machine.route == LinuxMachine::Route::Ssh ? tr("Host key") : tr("Host identity"),
                 fingerprint);
    if (verified)
        form->addRow(QString(), value(tr("Verified"), "linuxDetailsVerified"));
    if (machine.error != LinuxMachine::Error::None) {
        auto* error = value(errorText(machine), "linuxDetailsError");
        error->setProperty("remoteTone", QStringLiteral("danger"));
        form->addRow(error);
    }
    dialog->setBody(body);
    auto* close = dialog->addButton(tr("Close"), ket::ButtonVariant::Ghost);
    QObject::connect(close, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    const QPointer<LinuxMachineModel> guard(model);
    const QString id = machine.id;
    auto* disconnect = dialog->addButton(tr("Disconnect"));
    disconnect->setObjectName(QStringLiteral("linuxDetailsDisconnect"));
    disconnect->setEnabled(!offersConnect(machine));
    QObject::connect(disconnect, &QAbstractButton::clicked, dialog, [guard, id, dialog] {
        if (guard)
            guard->requestDisconnect(id);
        dialog->accept();
    });
    auto* connect = dialog->addButton(tr("Connect"), ket::ButtonVariant::Primary);
    connect->setObjectName(QStringLiteral("linuxDetailsConnect"));
    connect->setEnabled(offersConnect(machine));
    QObject::connect(connect, &QAbstractButton::clicked, dialog, [guard, id, dialog] {
        if (guard)
            guard->requestConnect(id);
        dialog->accept();
    });
    dialog->open();
}

} // namespace ketplus
