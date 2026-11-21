#include "remote/LinuxMachines.h"

#include "KetPlusProductVersion.h"
#include "control/ControlV2Client.h"
#include "remote/AccountEntitlements.h"
#include "remote/LinuxMachinesSection.h"
#include "ssh/SshMachineBackend.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>

namespace ketplus {

LinuxMachines::LinuxMachines(QObject* parent)
    : QObject(parent),
      client_(new remote::ControlV2Client(QUrl(QStringLiteral(KETPLUS_DEFAULT_CONTROL_URL)), this)),
      model_(new LinuxMachineModel(this)), presence_(new QTimer(this)) {
    connect(model_, &LinuxMachineModel::machinesChanged, this, &LinuxMachines::publishMachines);
    // After Forget old key the card opens again on that host, once, in one window.
    connect(model_, &LinuxMachineModel::sshConnectPrompted, this, [this](const QString& host) {
        if (auto* section = promptSection())
            section->openConnectCard(host);
    });
    connect(model_, &LinuxMachineModel::daemonManagementPrompted, this,
            [this](const QString& host) {
                if (auto* section = promptSection())
                    section->openConnectCard(host, true);
            });
    connect(client_, &remote::ControlV2Client::signInFinished, this, [this](const QString& error) {
        syncEntitlement();
        emit notice(error.isEmpty() ? tr("Signed in. You can pair a Linux machine.") : error,
                    !error.isEmpty());
        emit changed();
        if (error.isEmpty())
            refreshMachines();
    });
    // A revoked session is signed out: the Relay stops and its rows offer no Connect.
    connect(client_, &remote::ControlV2Client::credentialRevoked, this, [this] {
        syncEntitlement();
        emit changed();
    });
    presence_->setInterval(60 * 1000);
    connect(presence_, &QTimer::timeout, this, [this] {
        if (shown() && relay_ && relay_->enabled())
            refreshMachines();
        else if (shown())
            emit changed(); // "last seen" ages even without a new listing
    });
    syncEntitlement();
}

LinuxMachines::~LinuxMachines() {
    // The backends close with their owners; the sections stay without machines.
    disconnect(model_, nullptr, this, nullptr);
}

void LinuxMachines::publishMachines() {
    QStringList ids;
    QStringList names;
    for (const auto& machine : model_->machines()) {
        ids.append(machine.id);
        names.append(LinuxMachineModel::pickerLabel(machine));
    }
    emit machinesChanged(ids, names);
}

void LinuxMachines::syncEntitlement() {
    // The Relay is Pro: entitled only when the signed-in session names it (D-4: fail closed).
    const bool entitled = client_->signedIn() && relayEntitled(client_->entitlements());
    model_->setRelayEntitled(entitled);
    if (relay_)
        relay_->setEnabled(entitled);
}

void LinuxMachines::attachRelayBackend(LinuxRelayMachines::LinkFactory links) {
    delete relay_;
    relay_ = new LinuxRelayMachines(
        model_, links ? std::move(links) : LinuxRelayMachines::systemLinks(client_), this);
    presence_->start();
    // The listing arrives with start().
    syncEntitlement();
    emit changed();
}

void LinuxMachines::attachSshBackend(QString storePath, LinuxSshMachines::LinkFactory links) {
    delete ssh_;
    ssh_ = new LinuxSshMachines(model_, std::move(storePath), std::move(links), this);
    connect(ssh_, &LinuxSshMachines::notice, this, &LinuxMachines::notice);
    emit changed();
}

remote::MachineBackend* LinuxMachines::machineBackend(const QString& id) const {
    // The route the row shows: SSH, or the Relay while it is the fallback (ADR 0119).
    const auto* machine = model_->machine(id);
    auto* relay = relay_ ? relay_->backend(id) : nullptr;
    if (machine && machine->route == LinuxMachine::Route::Relay && relay)
        return relay;
    if (auto* backend = ssh_ ? ssh_->backend(id) : nullptr)
        return backend;
    return relay;
}

void LinuxMachines::start() {
    if (ssh_)
        ssh_->connectSaved();
    // A refreshed session may have gained or lost the Relay entitlement.
    syncEntitlement();
    emit changed();
    if (client_->signedIn())
        refreshMachines();
}

void LinuxMachines::signIn() {
    emit notice(tr("Opening account sign-in…"), false);
    client_->startSignIn([this](const QJsonValue& value, const QString& error) {
        if (!error.isEmpty()) {
            emit notice(error, true);
            return;
        }
        const QUrl page(value.toObject().value(QStringLiteral("authorizationUrl")).toString());
        if (!QDesktopServices::openUrl(page)) {
            client_->cancelSignIn();
            emit notice(tr("Could not open the account sign-in page."), true);
            return;
        }
        emit notice(tr("Finish sign-in in your browser, then return here."), false);
    });
}

void LinuxMachines::refreshMachines() {
    if (loading_ || !client_->signedIn())
        return;
    loading_ = true;
    emit changed();
    QPointer<LinuxMachines> guard(this);
    client_->listWorktrees([this, guard](const QJsonValue& value, const QString& error) {
        if (!guard)
            return;
        loading_ = false;
        if (!error.isEmpty()) {
            emit notice(error, true);
            emit changed();
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
        emit changed();
    });
}

void LinuxMachines::addSection(LinuxMachinesSection* section) {
    if (section && !sections_.contains(section))
        sections_.append(section);
}

void LinuxMachines::removeSection(LinuxMachinesSection* section) {
    sections_.removeAll(section);
    sections_.removeAll(nullptr);
}

void LinuxMachines::setPromptSection(LinuxMachinesSection* section) { promptSection_ = section; }

LinuxMachinesSection* LinuxMachines::promptSection() const {
    // The section Connections was last shown in, else one that is shown, else any.
    if (promptSection_ && promptSection_->isVisible())
        return promptSection_;
    for (const auto& section : sections_)
        if (section && section->isVisible())
            return section;
    if (promptSection_)
        return promptSection_;
    for (const auto& section : sections_)
        if (section)
            return section;
    return nullptr;
}

bool LinuxMachines::shown() const {
    for (const auto& section : sections_)
        if (section && section->isVisible())
            return true;
    return false;
}

} // namespace ketplus
