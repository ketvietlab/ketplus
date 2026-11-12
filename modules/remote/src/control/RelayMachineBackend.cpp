#include "control/RelayMachineBackend.h"
#include "control/ControlV2Client.h"
#include <QTimer>

// Connection states and reads of the Relay backend; RelayMachineCommands.cpp holds the writes.
namespace ketplus::remote {
namespace {
// Answers that mean this device may not use the relay any more: no retry until a new sign-in.
bool fatalCode(const QString& error) {
    static const QStringList codes{"invalid_access",
                                   "device_not_paired",
                                   "device_revoked",
                                   "machine_revoked",
                                   "Sign in before connecting to a remote host.",
                                   "Credential subject changed.",
                                   "Account changed."};
    return codes.contains(error);
}
// Failures of the link itself; any other error belongs to one request.
bool transient(const QString& error) {
    return error == QLatin1String("Control transport failed.") ||
           error == QLatin1String("Control refused the request.") ||
           error == QLatin1String("Invalid or oversized Control response.") ||
           error == QLatin1String("Credential refresh failed.") ||
           error == QLatin1String("Account changed while the request was pending.");
}
} // namespace

RelayMachineBackend::RelayMachineBackend(ControlV2Client* client, QString relayMachineId,
                                         Options options, QObject* parent)
    : MachineBackend(parent), client_(client), relayMachineId_(std::move(relayMachineId)),
      options_(std::move(options)), outbox_(options_.outboxFile), retry_(new QTimer(this)),
      events_(new QTimer(this)) {
    retry_->setSingleShot(true);
    connect(retry_, &QTimer::timeout, this, &RelayMachineBackend::probe);
    events_->setInterval(options_.eventPollMilliseconds);
    connect(events_, &QTimer::timeout, this, &RelayMachineBackend::pollEvents);
    if (!outbox_.load(&outboxError_))
        qWarning("Relay command outbox unavailable: %s", qUtf8Printable(outboxError_));
    if (client_)
        connect(client_, &ControlV2Client::credentialRevoked, this,
                [this] { failed(QStringLiteral("Account changed.")); });
}

RelayMachineBackend::~RelayMachineBackend() { ++generation_; }

QString RelayMachineBackend::machineId() const {
    const auto fingerprint = client_ ? client_->pinnedFingerprint(relayMachineId_) : QString{};
    return fingerprint.isEmpty() ? relayMachineId_ : fingerprint;
}

QStringList RelayMachineBackend::pendingCommands() const {
    QStringList ids;
    for (const auto& entry : outbox_.pending())
        ids.append(entry.id());
    return ids;
}

void RelayMachineBackend::setState(ConnectionState state, const QString& reason) {
    if (state_ == state && reason_ == reason)
        return;
    state_ = state;
    reason_ = reason;
    emit connectionStateChanged(state_, reason_);
}

void RelayMachineBackend::connectToRelay() {
    retry_->stop();
    attempts_ = 0;
    if (!outboxError_.isEmpty()) {
        setState(ConnectionState::Offline, outboxError_);
        return;
    }
    setState(everConnected_ ? ConnectionState::Reconnecting : ConnectionState::Connecting,
             everConnected_ ? QStringLiteral("Reconnecting to the relay.")
                            : QStringLiteral("Connecting to the relay."));
    probe();
}

void RelayMachineBackend::disconnectFromRelay() {
    ++generation_;
    retry_->stop();
    events_->stop();
    delivering_.clear();
    for (const auto& callback : std::exchange(callbacks_, {}))
        callback({}, QStringLiteral("Disconnected from the relay."));
    setState(ConnectionState::Offline, QStringLiteral("Disconnected from the relay."));
}

void RelayMachineBackend::probe() {
    if (!client_ || !client_->signedIn()) {
        failed(QStringLiteral("Sign in before connecting to a remote host."));
        return;
    }
    const auto generation = generation_;
    client_->listSessions([this, generation](const QJsonValue& value, const QString& error) {
        if (generation != generation_)
            return;
        if (!error.isEmpty() || !value.isArray()) {
            failed(error.isEmpty() ? QStringLiteral("Control refused the request.") : error);
            if (state_ != ConnectionState::Offline)
                scheduleProbe();
            return;
        }
        attempts_ = 0;
        everConnected_ = true;
        setState(ConnectionState::Connected, {});
        events_->start();
        resume();
    });
}

void RelayMachineBackend::scheduleProbe() {
    if (retry_->isActive())
        return;
    const auto shift = qMin(attempts_++, 16);
    retry_->start(int(qMin<qint64>(qint64(options_.initialBackoffMilliseconds) << shift,
                                   options_.maximumBackoffMilliseconds)));
}

bool RelayMachineBackend::fatal(const QString& error) const {
    return fatalCode(error) || !client_ || !client_->signedIn();
}

void RelayMachineBackend::failed(const QString& error) {
    if (fatal(error)) {
        // Revoked, signed out or another account: nothing in flight may reach the UI.
        ++generation_;
        retry_->stop();
        events_->stop();
        delivering_.clear();
        const auto reason = error == QLatin1String("Account changed.")
                                ? QStringLiteral("The relay sign-in ended; sign in again.")
                                : error;
        for (const auto& callback : std::exchange(callbacks_, {}))
            callback({}, reason);
        setState(ConnectionState::Offline, reason);
        return;
    }
    if (!transient(error))
        return;
    events_->stop();
    delivering_.clear();
    setState(everConnected_ ? ConnectionState::Reconnecting : ConnectionState::Connecting,
             QStringLiteral("The relay cannot be reached."));
    scheduleProbe();
}

bool RelayMachineBackend::requireConnected(const Callback& callback) const {
    if (state_ == ConnectionState::Connected && client_)
        return true;
    // A remote route fails a request made before Connected rather than holding it (ADR 0119).
    callback({}, reason_.isEmpty() ? QStringLiteral("This machine is not connected.") : reason_);
    return false;
}

MachineBackend::Callback RelayMachineBackend::fenced(Callback callback) {
    const auto generation = generation_;
    return [this, generation, callback = std::move(callback)](const QJsonObject& result,
                                                              const QString& error) {
        if (generation != generation_) {
            callback({}, reason_);
            return;
        }
        if (!error.isEmpty())
            failed(error);
        callback(result, error);
    };
}

void RelayMachineBackend::project(std::function<void(const QJsonObject&, const QString&)> done) {
    const auto generation = generation_;
    client_->projection(
        {{"verify_worktrees", options_.verifyWorktrees}},
        [this, generation, done = std::move(done)](const QJsonValue& value, const QString& error) {
            if (generation != generation_) {
                done({}, reason_);
                return;
            }
            if (!error.isEmpty()) {
                failed(error);
                done({}, error);
                return;
            }
            const auto projection = value.toObject();
            QJsonArray conversations, repositories, rejected;
            sessions_.clear();
            for (const auto& row : projection.value("conversations").toArray())
                if (row.toObject().value("machineId").toString() == relayMachineId_) {
                    conversations.append(row);
                    sessions_.insert(row.toObject().value("id").toString(), row.toObject());
                }
            for (const auto& group : projection.value("repositories").toArray())
                if (group.toObject().value("machineId").toString() == relayMachineId_)
                    repositories.append(group);
            for (const auto& row : projection.value("rejected").toArray())
                if (row.toObject().value("machineId").toString() == relayMachineId_)
                    rejected.append(row);
            repositories_ = repositories;
            done({{"repositories", repositories},
                  {"conversations", conversations},
                  {"rejected", rejected}},
                 {});
        });
}

QJsonObject RelayMachineBackend::sessionRow(const QString& id, qint64 generation) const {
    const auto row = sessions_.value(id);
    return row.value("generation").toInteger() == generation ? row : QJsonObject{};
}

void RelayMachineBackend::listRepositories(Callback callback) {
    if (!requireConnected(callback))
        return;
    project([callback = std::move(callback)](const QJsonObject& projection, const QString& error) {
        if (!error.isEmpty()) {
            callback({}, error);
            return;
        }
        // The neutral shape: the Relay names repositories by salted id and has no host path.
        QJsonArray repositories;
        for (const auto& value : projection.value("repositories").toArray()) {
            const auto group = value.toObject();
            repositories.append(QJsonObject{{"id", group.value("repositoryId")},
                                            {"label", group.value("label")},
                                            {"available", true},
                                            {"worktrees", group.value("worktrees")}});
        }
        callback({{"repositories", repositories}, {"rejected", projection.value("rejected")}}, {});
    });
}

void RelayMachineBackend::listWorktrees(const QString&, qint64, const QJsonObject& request,
                                        Callback callback) {
    if (!requireConnected(callback))
        return;
    project([request, callback = std::move(callback)](const QJsonObject& projection,
                                                      const QString& error) {
        if (!error.isEmpty()) {
            callback({}, error);
            return;
        }
        const auto repository = request.value("repository_id").toString();
        QJsonArray worktrees;
        for (const auto& value : projection.value("repositories").toArray()) {
            const auto group = value.toObject();
            if (!repository.isEmpty() && group.value("repositoryId").toString() != repository)
                continue;
            for (const auto& item : group.value("worktrees").toArray()) {
                auto worktree = item.toObject();
                worktree.insert("id", worktree.take("worktreeId"));
                worktree.insert("repository_id", group.value("repositoryId"));
                worktree.insert("repository_label", group.value("label"));
                worktrees.append(worktree);
            }
        }
        const auto offset = qMax(0, request.value("offset").toInt(0));
        const auto limit = qBound(1, request.value("limit").toInt(100), 100);
        QJsonArray page;
        for (qsizetype i = offset; i < worktrees.size() && page.size() < limit; ++i)
            page.append(worktrees.at(i));
        callback({{"worktrees", page},
                  {"total", worktrees.size()},
                  {"has_more", offset + page.size() < worktrees.size()}},
                 {});
    });
}

void RelayMachineBackend::listConversations(const QJsonObject&, Callback callback) {
    if (!requireConnected(callback))
        return;
    project([callback = std::move(callback)](const QJsonObject& projection, const QString& error) {
        callback(error.isEmpty() ? QJsonObject{{"sessions", projection.value("conversations")},
                                               {"rejected", projection.value("rejected")}}
                                 : QJsonObject{},
                 error);
    });
}

void RelayMachineBackend::transcriptWindow(const QString& sessionId, qint64 generation,
                                           const QJsonObject& request, Callback callback) {
    if (!requireConnected(callback))
        return;
    const auto row = sessionRow(sessionId, generation);
    if (row.isEmpty()) {
        callback({}, QStringLiteral("This conversation is not in the verified list."));
        return;
    }
    auto done = fenced(std::move(callback));
    client_->transcriptWindow(
        row, request,
        [this, sessionId, done = std::move(done)](const QJsonValue& value, const QString& error) {
            const auto page = value.toObject();
            // Follow the conversation's events from the snapshot the window was read at.
            if (error.isEmpty() && !watched_.contains(sessionId))
                watched_.insert(sessionId, page.value("snapshot_cursor").toInteger());
            done(page, error);
        });
}

void RelayMachineBackend::transcriptPage(const QString& sessionId, qint64 generation,
                                         const QJsonObject& request, Callback callback) {
    // An older page is a window before a cursor at the same snapshot.
    transcriptWindow(sessionId, generation, request, std::move(callback));
}

void RelayMachineBackend::transcriptLargeEvent(const QString&, qint64, const QJsonObject&,
                                               Callback callback) {
    callback({}, QStringLiteral("The Relay reassembles large events before delivery."));
}

} // namespace ketplus::remote
