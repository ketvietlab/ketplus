#include "machine/MachineCatalog.h"

#include <QJsonObject>
#include <QTimer>

#include <memory>

namespace ketplus {

namespace {

// Events after which the conversation list (titles, running state) is read again.
bool changesConversationList(const QString& type) {
    return type == QLatin1String("user.message") || type == QLatin1String("turn.started") ||
           type == QLatin1String("turn.completed") || type == QLatin1String("session.created") ||
           type == QLatin1String("session.title") || type == QLatin1String("session.deleted");
}

} // namespace

MachineCatalog::MachineCatalog(QObject* parent) : QObject(parent) {}

QString MachineCatalog::machineId() const {
    return backend_ != nullptr ? backend_->machineId() : QString{};
}

void MachineCatalog::setBackend(remote::MachineBackend* backend) {
    if (backend_ == backend)
        return;
    for (const auto& connection : std::as_const(connections_))
        disconnect(connection);
    connections_.clear();
    ++binding_; // drops answers still on their way from the previous machine
    backend_ = backend;
    clear();
    if (backend == nullptr) {
        setState(State::Offline, tr("This machine is not connected."));
        emit changed();
        return;
    }
    connections_.append(connect(backend, &remote::MachineBackend::connectionStateChanged, this,
                                [this](State state, const QString& reason) {
                                    const State previous = state_;
                                    setState(state, reason);
                                    if (state == State::Connecting || state == State::Offline)
                                        clear();
                                    if (state == State::Connected && previous != State::Connected)
                                        refresh();
                                    emit changed();
                                }));
    connections_.append(
        connect(backend, &remote::MachineBackend::catalogChanged, this, &MachineCatalog::refresh));
    connections_.append(connect(
        backend, &remote::MachineBackend::eventReceived, this, [this](const QJsonObject& event) {
            if (changesConversationList(
                    remote::windowEvent(event).value(QLatin1String("event_type")).toString()))
                refresh();
        }));
    connections_.append(
        connect(backend, &QObject::destroyed, this, [this] { setBackend(nullptr); }));
    setState(backend->connectionState(), backend->connectionReason());
    if (state_ == State::Connected)
        refresh();
    emit changed();
}

void MachineCatalog::setState(const State state, const QString& reason) {
    state_ = state;
    reason_ = state == State::Connected ? QString{} : reason;
}

void MachineCatalog::clear() {
    repositories_.clear();
    pending_.clear();
    conversations_.clear();
    error_.clear();
    loading_ = state_ == State::Connected || state_ == State::Connecting;
}

void MachineCatalog::refresh() {
    // Several triggers in one event-loop pass (a turn's start and its title) list once.
    if (backend_ == nullptr || refreshQueued_)
        return;
    refreshQueued_ = true;
    QTimer::singleShot(0, this, [this, binding = binding_] {
        refreshQueued_ = false;
        if (binding != binding_ || backend_ == nullptr || state_ != State::Connected)
            return;
        if (repositories_.isEmpty() && conversations_.isEmpty())
            loading_ = true;
        // Both answers replace the lists together, so a repository never shows without its
        // conversations or the other way round.
        struct Listing {
            QJsonObject repositories;
            QJsonObject conversations;
            QString error;
            int remaining{2};
        };
        auto listing = std::make_shared<Listing>();
        const auto finish = [this, binding, listing] {
            if (--listing->remaining > 0 || binding != binding_)
                return;
            loading_ = false;
            error_ = listing->error;
            if (listing->error.isEmpty()) {
                repositories_ = remote::repositoriesFromListing(listing->repositories);
                pending_ = remote::pendingWorktreesFromListing(listing->repositories);
                conversations_ = remote::conversationsFromListing(listing->conversations);
            }
            emit changed();
        };
        backend_->listRepositories(
            [listing, finish](const QJsonObject& result, const QString& error) {
                listing->repositories = result;
                if (!error.isEmpty())
                    listing->error = error;
                finish();
            });
        backend_->listConversations(
            {{QStringLiteral("deleted_sessions_page_version"), 1},
             {QStringLiteral("session_catalog_live_only"), true}},
            [listing, finish](const QJsonObject& result, const QString& error) {
                listing->conversations = result;
                if (!error.isEmpty() && listing->error.isEmpty())
                    listing->error = error;
                finish();
            });
    });
}

QString MachineCatalog::repositoryOf(const remote::MachineConversation& conversation) const {
    if (!conversation.repositoryId.isEmpty())
        return conversation.repositoryId;
    for (const auto& repository : repositories_)
        for (const auto& worktree : repository.worktrees)
            if (worktree.id == conversation.worktreeId)
                return repository.id;
    return {};
}

remote::MachineConversation MachineCatalog::conversation(const QString& sessionId) const {
    for (const auto& conversation : conversations_)
        if (conversation.sessionId == sessionId)
            return conversation;
    return {};
}

} // namespace ketplus
