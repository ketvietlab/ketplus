#include "remote/LinuxMachinesSection.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetIcons.h"
#include "KetTokens.h"

#include "control/ControlV2Client.h"
#include "remote/LinuxConnectDialog.h"
#include "remote/LinuxMachineRow.h"
#include "ssh/SshMachineBackend.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QStyle>
#include <QVBoxLayout>

namespace ketplus {

LinuxMachinesSection::LinuxMachinesSection(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("linuxMachinesSection"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(18); // the page's rhythm
    // The mock's section: "Linux machines · N trusted"; the page's Add menu starts pairing.
    auto* heading = new QHBoxLayout;
    auto* title = new QLabel(tr("Linux machines"), this);
    title->setObjectName(QStringLiteral("linuxMachinesTitle"));
    title->setProperty("remoteRole", QStringLiteral("sectionTitle"));
    heading->addWidget(title);
    heading->addStretch();
    count_ = new QLabel(tr("0 trusted"), this);
    count_->setObjectName(QStringLiteral("linuxMachinesCount"));
    count_->setProperty("remoteRole", QStringLiteral("sectionCount"));
    heading->addWidget(count_);
    outer->addLayout(heading);
    notice_ = new QLabel(this);
    notice_->setObjectName(QStringLiteral("linuxMachinesNotice"));
    notice_->setWordWrap(true);
    notice_->hide();
    outer->addWidget(notice_);
    auto* surface = new QFrame(this);
    surface->setObjectName(QStringLiteral("linuxMachinesSurface"));
    // One card for each machine, as the mock; a message is a card of its own.
    rows_ = new QVBoxLayout(surface);
    rows_->setContentsMargins(0, 0, 0, 0);
    rows_->setSpacing(18);
    outer->addWidget(surface);
    auto* footnoteRow = new QHBoxLayout;
    footnoteRow->setSpacing(8);
    auto* shield = new QLabel(this);
    shield->setFixedSize(13, 13);
    const auto inkShield = [this, shield] {
        shield->setPixmap(ket::Icons::pixmap(
            QStringLiteral("shield"), 13, devicePixelRatioF(),
            ket::Tokens::color(u"text-muted", ket::Appearance::instance().theme())));
    };
    inkShield();
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, shield, inkShield);
    footnoteRow->addWidget(shield, 0, Qt::AlignTop);
    auto* footnote =
        new QLabel(tr("Linux machines connect over your SSH access (free) or through the "
                      "ketPlus Relay (Pro). Verify the host fingerprint or pairing code before "
                      "trusting one."),
                   this);
    footnote->setObjectName(QStringLiteral("linuxMachinesFootnote"));
    footnote->setProperty("remoteRole", QStringLiteral("trustNote"));
    footnote->setWordWrap(true);
    footnoteRow->addWidget(footnote, 1);
    outer->addLayout(footnoteRow);
    // The rows' icons are drawn in the theme's ink.
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
            &LinuxMachinesSection::render);
    // The machines come with the shared set (setMachines), or with its own on first use.
}

LinuxMachines* LinuxMachinesSection::machines() const {
    if (!machines_ && !shared_) {
        auto* self = const_cast<LinuxMachinesSection*>(this);
        self->machines_ = new LinuxMachines(self);
        self->ownsMachines_ = true;
        self->connectMachines();
    }
    return machines_;
}

void LinuxMachinesSection::showEvent(QShowEvent* event) {
    machines(); // a section shown without a shared set lists its own
    QWidget::showEvent(event);
}

QStringList LinuxMachinesSection::machineIds() const {
    QStringList ids;
    for (const auto& machine : model()->machines())
        ids.append(machine.id);
    return ids;
}

void LinuxMachinesSection::setMachines(LinuxMachines* machines) {
    if (!machines || machines == machines_)
        return;
    if (machines_) {
        machines_->removeSection(this);
        disconnect(machines_, nullptr, this, nullptr);
        disconnect(machines_->model(), nullptr, this, nullptr);
        if (ownsMachines_)
            delete machines_.data();
    }
    machines_ = machines;
    ownsMachines_ = false;
    shared_ = true;
    connectMachines();
    machines_->publishMachines(); // the window's picker lists the shared machines at once
}

void LinuxMachinesSection::connectMachines() {
    machines_->addSection(this);
    connect(machines_, &LinuxMachines::machinesChanged, this,
            &LinuxMachinesSection::machinesChanged);
    connect(machines_->model(), &LinuxMachineModel::machinesChanged, this,
            &LinuxMachinesSection::render);
    connect(machines_->model(), &LinuxMachineModel::backendsAttachedChanged, this,
            &LinuxMachinesSection::render);
    connect(machines_, &LinuxMachines::changed, this, &LinuxMachinesSection::render);
    connect(machines_, &LinuxMachines::notice, this, &LinuxMachinesSection::setNotice);
    render();
}

LinuxMachineModel* LinuxMachinesSection::model() const {
    if (auto* set = machines())
        return set->model();
    if (!emptyModel_)
        const_cast<LinuxMachinesSection*>(this)->emptyModel_ =
            new LinuxMachineModel(const_cast<LinuxMachinesSection*>(this));
    return emptyModel_;
}

void LinuxMachinesSection::attachRelayBackend(LinuxRelayMachines::LinkFactory links) {
    if (auto* set = machines())
        set->attachRelayBackend(std::move(links));
}

void LinuxMachinesSection::attachSshBackend(QString storePath,
                                            LinuxSshMachines::LinkFactory links) {
    if (auto* set = machines())
        set->attachSshBackend(std::move(storePath), std::move(links));
}

remote::MachineBackend* LinuxMachinesSection::machineBackend(const QString& id) const {
    auto* set = machines();
    return set ? set->machineBackend(id) : nullptr;
}

void LinuxMachinesSection::activate() {
    // Connections is shown here: a prompt the machines raise opens in this window.
    auto* set = machines();
    if (!set)
        return; // the app's set is gone: the app is quitting
    set->setPromptSection(this);
    set->start();
}

void LinuxMachinesSection::setPairingEntryShown(const bool shown) {
    if (pairingEntryShown_ == shown)
        return;
    pairingEntryShown_ = shown;
    render();
}

void LinuxMachinesSection::setNotice(const QString& message, const bool error) {
    notice_->setText(message);
    notice_->setProperty("remoteTone",
                         error ? QStringLiteral("danger") : QStringLiteral("positive"));
    notice_->setVisible(!message.isEmpty());
    notice_->style()->unpolish(notice_);
    notice_->style()->polish(notice_);
}

void LinuxMachinesSection::render() {
    if (!machines_)
        return; // the app's set is gone: the app is quitting
    while (auto* item = rows_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    auto* shown = model();
    auto* account = machines_->client();
    const auto& machines = shown->machines();
    count_->setText(tr("%1 trusted").arg(machines.size()));
    const auto message = [this](const QString& text) {
        auto* box = new QFrame(this);
        box->setProperty("remoteRole", QStringLiteral("surface"));
        auto* layout = new QVBoxLayout(box);
        layout->setContentsMargins(22, 20, 22, 20);
        layout->setSpacing(12);
        auto* label = new QLabel(text, box);
        label->setProperty("remoteRole", QStringLiteral("emptyText"));
        label->setWordWrap(true);
        layout->addWidget(label);
        rows_->addWidget(box);
        return layout;
    };
    const auto addRows = [this, shown, &machines] {
        // A device row as the phones above: icon, name, its state and route, and its actions.
        for (const auto& machine : machines)
            rows_->addWidget(makeLinuxMachineRow(machine, shown, this));
    };
    // SSH needs no account, so it is offered signed out too, once a backend can carry it.
    const bool ssh = shown->backendsAttached();
    const auto addConnectButton = [this](QBoxLayout* layout, const ket::ButtonVariant variant) {
        auto* connectButton = new ket::Button(tr("Connect a Linux machine"), variant, this);
        connectButton->setObjectName(QStringLiteral("linuxMachinesConnect"));
        connect(connectButton, &QAbstractButton::clicked, this,
                &LinuxMachinesSection::beginPairing);
        layout->addWidget(connectButton, 0, Qt::AlignLeft);
    };
    if (!account->signedIn()) {
        addRows();
        auto* layout = message(
            ssh ? tr("Connect over SSH with your own SSH keys; no account is needed. To pair "
                     "through the ketPlus Relay, sign in with the KetSuite account the Linux "
                     "daemon uses.")
                : tr("Sign in with the KetSuite account the Linux daemon uses to pair it through "
                     "the ketPlus Relay."));
        auto* actions = new QHBoxLayout;
        actions->setSpacing(8);
        if (ssh && pairingEntryShown_)
            addConnectButton(actions, ket::ButtonVariant::Primary);
        auto* signInButton = new ket::Button(
            ssh ? tr("Sign in for the Relay") : tr("Sign in to connect a Linux machine"),
            ssh && pairingEntryShown_ ? ket::ButtonVariant::Secondary : ket::ButtonVariant::Primary,
            this);
        signInButton->setObjectName(QStringLiteral("linuxMachinesSignIn"));
        connect(signInButton, &QAbstractButton::clicked, machines_, &LinuxMachines::signIn);
        actions->addWidget(signInButton, 0, Qt::AlignLeft);
        actions->addStretch(1);
        layout->addLayout(actions);
        return;
    }
    if (machines_->loading() && machines.isEmpty()) {
        message(tr("Loading trusted machines…"));
        return;
    }
    if (machines.isEmpty()) {
        const QString how =
            ssh ? tr("No Linux machines yet. Connect one over SSH with a host from your "
                     "~/.ssh/config, or pair one through the ketPlus Relay with the one-time "
                     "link from ketplusctl pairing create.")
                : tr("No Linux machines yet. Run ketplusctl pairing create on the Linux machine, "
                     "then paste the one-time pairing link it shows.");
        if (pairingEntryShown_)
            addConnectButton(message(how), ket::ButtonVariant::Secondary);
        else
            message(how + QLatin1Char(' ') + tr("Choose Add › Connect a Linux machine to start."));
        return;
    }
    addRows();
    if (pairingEntryShown_) {
        auto* footer = new QWidget(this);
        auto* layout = new QHBoxLayout(footer);
        layout->setContentsMargins(0, 0, 0, 0);
        addConnectButton(layout, ket::ButtonVariant::Secondary);
        rows_->addWidget(footer);
    }
}

LinuxMachinesSection::~LinuxMachinesSection() {
    if (machines_) {
        machines_->removeSection(this);
        disconnect(machines_, nullptr, this, nullptr);
        disconnect(machines_->model(), nullptr, this, nullptr);
    }
    delete connectDialog_.data();
}

void LinuxMachinesSection::beginPairing() { openConnectCard({}); }

void LinuxMachinesSection::openConnectCard(const QString& sshHost, bool manageDaemon) {
    if (connectDialog_)
        return;
    // SSH needs no account, so the card opens signed out too; its Relay mode offers sign-in.
    QPointer<LinuxMachines> set = machines();
    if (!set)
        return;
    connectDialog_ = openLinuxConnectDialog(this, model(), set->client(),
                                            {[set] {
                                                 if (set)
                                                     set->signIn();
                                             },
                                             [this, set](const QString& message) {
                                                 setNotice(message);
                                                 if (set)
                                                     set->refreshMachines();
                                             }},
                                            LinuxConnectMode::Ssh, sshHost, manageDaemon);
}

} // namespace ketplus
