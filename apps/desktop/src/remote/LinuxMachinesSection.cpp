#include "remote/LinuxMachinesSection.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetIcons.h"
#include "KetTokens.h"

#include "KetPlusProductVersion.h"
#include "control/ControlV2Client.h"
#include "remote/AccountEntitlements.h"
#include "remote/LinuxConnectDialog.h"
#include "remote/LinuxMachineRow.h"
#include "ssh/SshMachineBackend.h"

#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace ketplus {

LinuxMachinesSection::LinuxMachinesSection(QWidget* parent)
    : QWidget(parent), client_(new remote::ControlV2Client(
                           QUrl(QStringLiteral(KETPLUS_DEFAULT_CONTROL_URL)), this)) {
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
    connect(client_, &remote::ControlV2Client::signInFinished, this, [this](const QString& error) {
        syncEntitlement();
        setNotice(error.isEmpty() ? tr("Signed in. You can pair a Linux machine.") : error,
                  !error.isEmpty());
        render();
        if (error.isEmpty())
            refreshMachines();
    });
    // A revoked session is signed out: the Relay stops and its rows offer no Connect.
    connect(client_, &remote::ControlV2Client::credentialRevoked, this, [this] {
        syncEntitlement();
        render();
    });
    presence_ = new QTimer(this);
    presence_->setInterval(60 * 1000);
    connect(presence_, &QTimer::timeout, this, [this] {
        if (isVisible() && relay_ && relay_->enabled())
            refreshMachines();
        else if (isVisible())
            render(); // "last seen" ages even without a new listing
    });
    // The rows' icons are drawn in the theme's ink.
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
            &LinuxMachinesSection::render);
    setModel(new LinuxMachineModel(this));
}

QStringList LinuxMachinesSection::machineIds() const {
    QStringList ids;
    for (const auto& machine : model_->machines())
        ids.append(machine.id);
    return ids;
}

void LinuxMachinesSection::setModel(LinuxMachineModel* model) {
    if (!model || model == model_)
        return;
    if (model_) {
        disconnect(model_, nullptr, this, nullptr);
        if (model_->parent() == this)
            model_->deleteLater();
    }
    model_ = model;
    connect(model_, &LinuxMachineModel::machinesChanged, this, [this] {
        render();
        QStringList ids;
        QStringList names;
        for (const auto& machine : model_->machines()) {
            ids.append(machine.id);
            names.append(LinuxMachineModel::pickerLabel(machine));
        }
        emit machinesChanged(ids, names);
    });
    connect(model_, &LinuxMachineModel::sshConnectPrompted, this,
            &LinuxMachinesSection::openConnectCard);
    connect(model_, &LinuxMachineModel::backendsAttachedChanged, this,
            &LinuxMachinesSection::render);
    connect(model_, &QObject::destroyed, this, [this] {
        model_ = nullptr;
        setModel(new LinuxMachineModel(this));
    });
    syncEntitlement();
    render();
}

void LinuxMachinesSection::syncEntitlement() {
    // The Relay is Pro: entitled only when the signed-in session names it (D-4: fail closed).
    const bool entitled = client_->signedIn() && relayEntitled(client_->entitlements());
    model_->setRelayEntitled(entitled);
    if (relay_)
        relay_->setEnabled(entitled);
}

void LinuxMachinesSection::attachRelayBackend(LinuxRelayMachines::LinkFactory links) {
    delete relay_;
    relay_ = new LinuxRelayMachines(
        model_, links ? std::move(links) : LinuxRelayMachines::systemLinks(client_), this);
    presence_->start();
    // The listing arrives when Connections opens (activate()).
    syncEntitlement();
    render();
}

void LinuxMachinesSection::attachSshBackend(QString storePath,
                                            LinuxSshMachines::LinkFactory links) {
    delete ssh_;
    ssh_ = new LinuxSshMachines(model_, std::move(storePath), std::move(links), this);
    connect(ssh_, &LinuxSshMachines::notice, this, &LinuxMachinesSection::setNotice);
    render();
}

remote::MachineBackend* LinuxMachinesSection::machineBackend(const QString& id) const {
    // The route the row shows: SSH, or the Relay while it is the fallback (ADR 0119).
    const auto* machine = model_ ? model_->machine(id) : nullptr;
    auto* relay = relay_ ? relay_->backend(id) : nullptr;
    if (machine && machine->route == LinuxMachine::Route::Relay && relay)
        return relay;
    if (auto* backend = ssh_ ? ssh_->backend(id) : nullptr)
        return backend;
    return relay;
}

void LinuxMachinesSection::activate() {
    if (ssh_)
        ssh_->connectSaved();
    // A refreshed session may have gained or lost the Relay entitlement.
    syncEntitlement();
    render();
    if (client_->signedIn())
        refreshMachines();
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
    while (auto* item = rows_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto& machines = model_->machines();
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
    const auto addRows = [this, &machines] {
        // A device row as the phones above: icon, name, its state and route, and its actions.
        for (const auto& machine : machines)
            rows_->addWidget(makeLinuxMachineRow(machine, model_, this));
    };
    // SSH needs no account, so it is offered signed out too, once a backend can carry it.
    const bool ssh = model_->backendsAttached();
    const auto addConnectButton = [this](QBoxLayout* layout, const ket::ButtonVariant variant) {
        auto* connectButton = new ket::Button(tr("Connect a Linux machine"), variant, this);
        connectButton->setObjectName(QStringLiteral("linuxMachinesConnect"));
        connect(connectButton, &QAbstractButton::clicked, this,
                &LinuxMachinesSection::beginPairing);
        layout->addWidget(connectButton, 0, Qt::AlignLeft);
    };
    if (!client_->signedIn()) {
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
        connect(signInButton, &QAbstractButton::clicked, this, &LinuxMachinesSection::signIn);
        actions->addWidget(signInButton, 0, Qt::AlignLeft);
        actions->addStretch(1);
        layout->addLayout(actions);
        return;
    }
    if (loading_ && machines.isEmpty()) {
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

void LinuxMachinesSection::signIn() {
    setNotice(tr("Opening account sign-in…"));
    client_->startSignIn([this](const QJsonValue& value, const QString& error) {
        if (!error.isEmpty()) {
            setNotice(error, true);
            return;
        }
        const QUrl page(value.toObject().value(QStringLiteral("authorizationUrl")).toString());
        if (!QDesktopServices::openUrl(page)) {
            client_->cancelSignIn();
            setNotice(tr("Could not open the account sign-in page."), true);
            return;
        }
        setNotice(tr("Finish sign-in in your browser, then return here."));
    });
}

LinuxMachinesSection::~LinuxMachinesSection() {
    if (model_)
        disconnect(model_, nullptr, this, nullptr);
    delete connectDialog_.data();
}

void LinuxMachinesSection::beginPairing() { openConnectCard({}); }

void LinuxMachinesSection::openConnectCard(const QString& sshHost) {
    if (connectDialog_)
        return;
    // SSH needs no account, so the card opens signed out too; its Relay mode offers sign-in.
    connectDialog_ = openLinuxConnectDialog(this, model_, client_,
                                            {[this] { signIn(); },
                                             [this](const QString& message) {
                                                 setNotice(message);
                                                 refreshMachines();
                                             }},
                                            LinuxConnectMode::Ssh, sshHost);
}

void LinuxMachinesSection::refreshMachines() {
    if (loading_ || !client_->signedIn())
        return;
    loading_ = true;
    render();
    client_->listWorktrees([this](const QJsonValue& value, const QString& error) {
        loading_ = false;
        if (!error.isEmpty()) {
            setNotice(error, true);
            render();
            return;
        }
        // ADR 0119: each host is named by the fingerprint pinned when it was paired.
        // Each worktree carries its host's display name and when the host last refreshed it
        // (its heartbeat): the newest one is when the host was last seen.
        QList<LinuxRelayListing> listed;
        QHash<QString, qsizetype> seen;
        for (const auto& item : value.toArray()) {
            const auto worktree = item.toObject();
            const auto id = worktree.value(QStringLiteral("machineId")).toString();
            if (id.isEmpty())
                continue;
            const auto updated = QDateTime::fromString(
                worktree.value(QStringLiteral("updatedAt")).toString(), Qt::ISODateWithMs);
            const auto name = worktree.value(QStringLiteral("machineDisplayName")).toString();
            if (!seen.contains(id)) {
                seen.insert(id, listed.size());
                listed.append({id, client_->pinnedFingerprint(id), name, updated});
                continue;
            }
            auto& listing = listed[seen.value(id)];
            if (updated.isValid() && (!listing.lastSeen.isValid() || updated > listing.lastSeen))
                listing.lastSeen = updated;
            if (listing.name.isEmpty())
                listing.name = name;
        }
        if (relay_)
            relay_->setListed(listed);
        else
            model_->setRelayMachines(listed);
        render();
    });
}

} // namespace ketplus
