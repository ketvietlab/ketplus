#include "remote/LinuxRelayMachines.h"

#include "control/RelayMachineBackend.h"

#include <QCryptographicHash>
#include <QStandardPaths>

namespace ketplus {

namespace {

using remote::MachineBackend;
using remote::RelayMachineBackend;

class SystemRelayLink final : public LinuxRelayLink {
  public:
    SystemRelayLink(remote::ControlV2Client* client, const QString& relayId)
        : backend_(new RelayMachineBackend(
              client, relayId, {.outboxFile = LinuxRelayMachines::outboxFile(relayId)}, this)) {
        connect(backend_, &MachineBackend::connectionStateChanged, this,
                &LinuxRelayLink::pendingChanged);
        connect(backend_, &RelayMachineBackend::commandCompleted, this,
                &LinuxRelayLink::pendingChanged);
    }

    MachineBackend* backend() override { return backend_; }
    void connectToRelay() override { backend_->connectToRelay(); }
    void disconnectFromRelay() override { backend_->disconnectFromRelay(); }
    [[nodiscard]] int pendingCommands() const override {
        return static_cast<int>(backend_->pendingCommands().size());
    }

  private:
    RelayMachineBackend* backend_;
};

LinuxMachine::State stateOf(const MachineBackend::ConnectionState state) {
    switch (state) {
    case MachineBackend::ConnectionState::Connecting:
        return LinuxMachine::State::Connecting;
    case MachineBackend::ConnectionState::Connected:
        return LinuxMachine::State::Connected;
    case MachineBackend::ConnectionState::Reconnecting:
        return LinuxMachine::State::Reconnecting;
    case MachineBackend::ConnectionState::Offline:
        break;
    }
    return LinuxMachine::State::Offline;
}

} // namespace

LinuxRelayMachines::LinkFactory LinuxRelayMachines::systemLinks(remote::ControlV2Client* client) {
    return [client](const QString& relayId) -> std::unique_ptr<LinuxRelayLink> {
        return std::make_unique<SystemRelayLink>(client, relayId);
    };
}

QString LinuxRelayMachines::outboxFile(const QString& relayId) {
    // The relay id is the Relay's own name for the host; hashed so it is a safe file name.
    const auto name =
        QCryptographicHash::hash(relayId.toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/Ketsuite/KetPlus/relay/outbox-%1.json").arg(QString::fromLatin1(name));
}

LinuxRelayMachines::LinuxRelayMachines(LinuxMachineModel* model, LinkFactory links, QObject* parent)
    : QObject(parent), model_(model), links_(std::move(links)) {
    if (!model_)
        return;
    const auto relayRoute = [this](const QString& id) {
        const auto* machine = model_->machine(id);
        // A host on both routes is one machine with SSH preferred: the SSH backend acts on it.
        return machine && machine->route == LinuxMachine::Route::Relay ? machine->relayId
                                                                       : QString();
    };
    connect(model_, &LinuxMachineModel::connectRequested, this,
            [this, relayRoute](const QString& id) {
                const auto relayId = relayRoute(id);
                auto* link = relayLinks_.value(relayId);
                if (!link || !enabled_)
                    return;
                stopped_.remove(relayId);
                link->connectToRelay();
                publish(relayId);
            });
    connect(model_, &LinuxMachineModel::disconnectRequested, this,
            [this, relayRoute](const QString& id) {
                const auto relayId = relayRoute(id);
                auto* link = relayLinks_.value(relayId);
                if (!link)
                    return;
                stopped_.insert(relayId);
                link->disconnectFromRelay();
                publish(relayId);
            });
}

LinuxRelayMachines::~LinuxRelayMachines() {
    for (auto* link : std::as_const(relayLinks_))
        delete link;
}

void LinuxRelayMachines::setListed(const QList<LinuxRelayListing>& listed) {
    if (!model_)
        return;
    model_->setRelayMachines(listed);
    QSet<QString> keep;
    for (const auto& listing : listed) {
        keep.insert(listing.relayId);
        ensure(listing.relayId);
    }
    for (auto it = relayLinks_.begin(); it != relayLinks_.end();) {
        if (keep.contains(it.key())) {
            ++it;
            continue;
        }
        stopped_.remove(it.key());
        delete it.value();
        it = relayLinks_.erase(it);
    }
    publish();
}

void LinuxRelayMachines::setEnabled(const bool enabled) {
    if (enabled_ == enabled) {
        publish();
        return;
    }
    enabled_ = enabled;
    for (auto it = relayLinks_.cbegin(); it != relayLinks_.cend(); ++it) {
        if (!enabled_)
            it.value()->disconnectFromRelay();
        else if (!stopped_.contains(it.key()))
            it.value()->connectToRelay();
    }
    publish();
}

void LinuxRelayMachines::ensure(const QString& relayId) {
    if (relayId.isEmpty() || relayLinks_.contains(relayId) || !links_)
        return;
    auto link = links_(relayId);
    if (!link)
        return;
    auto* raw = link.release();
    relayLinks_.insert(relayId, raw);
    connect(raw->backend(), &MachineBackend::connectionStateChanged, this,
            [this, relayId] { publish(relayId); });
    connect(raw, &LinuxRelayLink::pendingChanged, this, [this, relayId] { publish(relayId); });
    if (enabled_)
        raw->connectToRelay();
}

remote::MachineBackend* LinuxRelayMachines::backend(const QString& machineId) const {
    auto* link = relayLinks_.value(relayIdOf(machineId));
    return link ? link->backend() : nullptr;
}

QString LinuxRelayMachines::relayIdOf(const QString& machineId) const {
    if (!model_)
        return {};
    const auto* machine = model_->machine(machineId);
    return machine ? machine->relayId : QString();
}

void LinuxRelayMachines::publish() {
    for (auto it = relayLinks_.cbegin(); it != relayLinks_.cend(); ++it)
        publish(it.key());
}

void LinuxRelayMachines::publish(const QString& relayId) {
    auto* link = relayLinks_.value(relayId);
    if (!model_ || !link)
        return;
    if (!enabled_) {
        // Not ketPlus Pro (or signed out): the route is shown, but cannot be used.
        model_->setRelayState(relayId, LinuxMachine::State::Offline, LinuxMachine::Error::None, {},
                              link->pendingCommands(), false);
        return;
    }
    const auto* backend = link->backend();
    const auto state = stateOf(backend->connectionState());
    const bool failed = state == LinuxMachine::State::Offline && !stopped_.contains(relayId) &&
                        !backend->connectionReason().isEmpty();
    model_->setRelayState(
        relayId, state, failed ? LinuxMachine::Error::Other : LinuxMachine::Error::None,
        failed ? backend->connectionReason() : QString(), link->pendingCommands(), true);
}

} // namespace ketplus
