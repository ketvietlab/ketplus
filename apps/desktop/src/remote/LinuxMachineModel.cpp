#include "remote/LinuxMachineModel.h"

#include <QCoreApplication>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <utility>

namespace ketplus {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("LinuxMachineModel", text); }

} // namespace

const LinuxMachine* LinuxMachineModel::machine(const QString& id) const {
    for (const auto& machine : machines_)
        if (machine.id == id)
            return &machine;
    return nullptr;
}

void LinuxMachineModel::setMachines(QList<LinuxMachine> machines) {
    ssh_.clear();
    relay_.clear();
    for (auto& machine : machines)
        (machine.route == LinuxMachine::Route::Ssh ? ssh_ : relay_).append(std::move(machine));
    merge();
}

void LinuxMachineModel::upsert(const LinuxMachine& machine) {
    // Each route keeps its own row: a host on both routes is merged, not overwritten.
    auto& own = machine.route == LinuxMachine::Route::Ssh ? ssh_ : relay_;
    const auto same = std::find_if(own.begin(), own.end(), [&machine](const LinuxMachine& known) {
        return known.id == machine.id;
    });
    if (same != own.end())
        *same = machine;
    else
        own.append(machine);
    merge();
}

void LinuxMachineModel::setSshMachines(QList<LinuxMachine> machines) {
    for (auto& machine : machines)
        machine.route = LinuxMachine::Route::Ssh;
    ssh_ = std::move(machines);
    merge();
}

QString LinuxMachineModel::relayMachineId(const LinuxRelayListing& listing) {
    return listing.fingerprint.isEmpty() ? QStringLiteral("relay:") + listing.relayId
                                         : listing.fingerprint;
}

void LinuxMachineModel::setRelayMachines(const QList<LinuxRelayListing>& listed) {
    QList<LinuxMachine> next;
    QSet<QString> relayIds;
    for (const auto& listing : listed) {
        if (listing.relayId.isEmpty() || relayIds.contains(listing.relayId))
            continue;
        relayIds.insert(listing.relayId);
        const QString id = relayMachineId(listing);
        // Listed twice under one identity: one machine.
        if (std::any_of(next.cbegin(), next.cend(),
                        [&id](const LinuxMachine& machine) { return machine.id == id; }))
            continue;
        const auto known =
            std::find_if(relay_.cbegin(), relay_.cend(),
                         [&id](const LinuxMachine& machine) { return machine.id == id; });
        if (known != relay_.cend()) {
            next.append(*known);
            next.last().relayId = listing.relayId;
            next.last().name = listing.name.isEmpty() ? listing.relayId : listing.name;
            next.last().hostLastSeen = listing.lastSeen;
            continue;
        }
        // Listed by the Relay's worktree index, so the Relay reached it; the backend reports
        // live state.
        LinuxMachine relay;
        relay.id = id;
        relay.name = listing.name.isEmpty() ? listing.relayId : listing.name;
        relay.hostLastSeen = listing.lastSeen;
        // "Trusted" only beside a pinned identity, never beside "Not verified yet".
        relay.detail = listing.fingerprint.isEmpty() ? tr("Linux · Paired") : tr("Linux · Trusted");
        relay.route = LinuxMachine::Route::Relay;
        relay.state = LinuxMachine::State::Connected;
        relay.fingerprint = listing.fingerprint;
        relay.relayId = listing.relayId;
        next.append(relay);
    }
    relay_ = std::move(next);
    merge();
}

void LinuxMachineModel::setRelayState(const QString& relayId, const LinuxMachine::State state,
                                      const LinuxMachine::Error error, const QString& errorText,
                                      const int pendingCommands, const bool connectable) {
    for (auto& machine : relay_) {
        if (machine.relayId != relayId)
            continue;
        machine.state = state;
        machine.error = error;
        machine.errorText = errorText;
        machine.pendingCommands = pendingCommands;
        machine.connectable = connectable;
    }
    merge();
}

void LinuxMachineModel::merge() {
    QList<LinuxMachine> next;
    QSet<QString> sshIds;
    for (auto machine : std::as_const(ssh_)) {
        if (sshIds.contains(machine.id))
            continue;
        sshIds.insert(machine.id);
        // The same host over the Relay: one machine, reached over SSH, the Relay its attribute.
        machine.relayId.clear();
        for (const auto& relay : std::as_const(relay_))
            if (relay.id == machine.id && !relay.relayId.isEmpty()) {
                machine.relayId = relay.relayId;
                break;
            }
        next.append(machine);
    }
    for (const auto& relay : std::as_const(relay_))
        if (!sshIds.contains(relay.id))
            next.append(relay);
    if (machines_ == next)
        return;
    machines_ = std::move(next);
    emit machinesChanged();
}

void LinuxMachineModel::setBackendsAttached(const bool attached) {
    if (backendsAttached_ == attached)
        return;
    backendsAttached_ = attached;
    emit backendsAttachedChanged(attached);
}

void LinuxMachineModel::setRelayEntitled(const bool entitled) {
    if (relayEntitled_ == entitled)
        return;
    relayEntitled_ = entitled;
    emit relayEntitledChanged(entitled);
}

void LinuxMachineModel::presentHostKey(const QString& host, const QString& fingerprint) {
    emit hostKeyPresented(host, fingerprint);
}

void LinuxMachineModel::reportSshFailure(const QString& host, const LinuxMachine::Error error,
                                         const QString& text) {
    emit sshFailed(host, error, text);
}

void LinuxMachineModel::reportSshConnected(const QString& host) { emit sshConnected(host); }

QString LinuxMachineModel::stateText(const LinuxMachine::State state) {
    switch (state) {
    case LinuxMachine::State::Connecting:
        return tr("Connecting…");
    case LinuxMachine::State::Connected:
        return tr("Connected");
    case LinuxMachine::State::Reconnecting:
        return tr("Reconnecting…");
    case LinuxMachine::State::Offline:
        break;
    }
    return tr("Offline");
}

QString LinuxMachineModel::routeText(const LinuxMachine::Route route) {
    return route == LinuxMachine::Route::Ssh ? tr("SSH") : tr("Relay");
}

QString LinuxMachineModel::errorMessage(const LinuxMachine::Error error, const QString& host,
                                        const QString& text) {
    switch (error) {
    case LinuxMachine::Error::None:
        return {};
    case LinuxMachine::Error::HostKeyMismatch:
        // Each command sits on a line of its own: it never wraps at its hyphen and copies whole.
        return tr("The host key of %1 does not match the one in known_hosts, so ketPlus did not "
                  "connect. If the machine was reinstalled, check its new key on the machine, "
                  "remove the old entry and connect again:\nssh-keygen -R %1")
            .arg(host);
    case LinuxMachine::Error::AuthenticationFailed:
        return tr("%1 did not accept your SSH keys. ketPlus does not use SSH passwords. Name "
                  "your key for this host in ~/.ssh/config, or add it to the SSH agent and "
                  "connect again:\nssh-add")
            .arg(host);
    case LinuxMachine::Error::DaemonNotInstalled:
        return tr("The ketPlus daemon is not installed on %1. Install it for this user on the "
                  "machine, then connect again.")
            .arg(host);
    case LinuxMachine::Error::IdentityChanged:
        return tr("The ketPlus daemon on %1 now names another host identity, so ketPlus did not "
                  "connect. If the daemon was reinstalled, check its identity on the machine "
                  "(ketplusctl status), then trust the new identity in Details.")
            .arg(host);
    case LinuxMachine::Error::Other:
        break;
    }
    return text.isEmpty() ? tr("Could not connect to %1.").arg(host) : text;
}

QString LinuxMachineModel::errorMessage(const LinuxMachine& machine) {
    if (machine.error == LinuxMachine::Error::HostKeyMismatch && machine.canForgetHostKey)
        return tr("The host key of %1 changed since you accepted it in ketPlus, so ketPlus did "
                  "not connect. If the machine was reinstalled, choose Forget old key in Details "
                  "and compare the new key with the one on the machine before you accept it.")
            .arg(machine.name);
    return errorMessage(machine.error, machine.name, machine.errorText);
}

bool LinuxMachineModel::hostAway(const LinuxMachine& machine, const QDateTime& now) {
    return machine.route == LinuxMachine::Route::Relay && machine.hostLastSeen.isValid() &&
           machine.hostLastSeen.secsTo(now) > hostAwayAfterSeconds;
}

QString LinuxMachineModel::presenceText(const LinuxMachine& machine, const QDateTime& now) {
    if (machine.route != LinuxMachine::Route::Relay || !machine.hostLastSeen.isValid())
        return {};
    if (!hostAway(machine, now))
        return tr("Host online");
    const qint64 minutes = machine.hostLastSeen.secsTo(now) / 60;
    if (minutes < 60)
        return tr("Host offline, last seen %1 min ago").arg(minutes);
    if (minutes < 48 * 60)
        return tr("Host offline, last seen %1 h ago").arg(minutes / 60);
    return tr("Host offline, last seen %1")
        .arg(QLocale().toString(machine.hostLastSeen.toLocalTime().date(), QLocale::ShortFormat));
}

QString LinuxMachineModel::pickerLabel(const LinuxMachine& machine) {
    QString state = machine.error == LinuxMachine::Error::None ? stateText(machine.state)
                                                               : tr("Needs attention");
    // Connected over the Relay means the Relay answers; say so when the host itself is away.
    if (machine.error == LinuxMachine::Error::None &&
        machine.state == LinuxMachine::State::Connected &&
        hostAway(machine, QDateTime::currentDateTimeUtc()))
        state = tr("Host offline");
    return tr("%1 · %2 · %3").arg(machine.name, routeText(machine.route), state);
}

QString validateSshHost(const QString& host) {
    // A leading '-' would reach ssh as an option, so it is never a host.
    static const QRegularExpression pattern(QStringLiteral(
        R"(^(?:[A-Za-z0-9_][A-Za-z0-9._-]*@)?([A-Za-z0-9_][A-Za-z0-9._-]*|\[[0-9A-Fa-f:.]+\])(?::(\d{1,5}))?$)"));
    const auto match = pattern.match(host);
    if (host.isEmpty() || !match.hasMatch())
        return tr("Enter a host alias from ~/.ssh/config or user@host, with :port if needed.");
    if (const auto port = match.captured(2); !port.isEmpty()) {
        const int value = port.toInt();
        if (value < 1 || value > 65535)
            return tr("Use a port between 1 and 65535.");
    }
    return {};
}

} // namespace ketplus
