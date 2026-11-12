#include "control/ControlV2Client.h"
#include "control/RelayMachineBackend.h"
#include <QDateTime>
#include <QTimer>
#include <QUuid>

// Writes of the Relay backend: every command goes through the durable outbox, and its result is
// applied once, after the host signature is verified (RelayMachineBackend.cpp holds the reads).
namespace ketplus::remote {
namespace {
// Errors that leave a command in the outbox for the next connection.
bool linkFailure(const QString& error) {
    return error == QLatin1String("Control transport failed.") ||
           error == QLatin1String("Control refused the request.") ||
           error == QLatin1String("Invalid or oversized Control response.") ||
           error == QLatin1String("Credential refresh failed.") ||
           error == QLatin1String("Account changed while the request was pending.") ||
           error == QLatin1String("Account changed.") || error == QLatin1String("invalid_access") ||
           error == QLatin1String("Sign in before connecting to a remote host.");
}
} // namespace

void RelayMachineBackend::sessionCommand(const QString& kind, const QString& sessionId,
                                         qint64 generation, const QJsonObject& plaintext,
                                         Callback callback) {
    if (hold(callback, [this, kind, sessionId, generation, plaintext](Callback again) {
            sessionCommand(kind, sessionId, generation, plaintext, std::move(again));
        }))
        return;
    const auto row = sessionRow(sessionId, generation);
    if (row.isEmpty()) {
        callback({}, QStringLiteral("This conversation is not in the verified list."));
        return;
    }
    if (!watched_.contains(sessionId))
        watched_.insert(sessionId, row.value("lastEventCursor").toInteger());
    const auto token = QUuid::createUuid().toString(QUuid::Id128);
    const QJsonObject intent{{"id", "command_" + token},
                             {"idempotencyKey", "command_idempotency_" + token},
                             {"workspaceId", row.value("workspaceId")},
                             {"machineId", relayMachineId_},
                             {"sessionId", sessionId},
                             {"sessionGeneration", generation},
                             {"capability", kind},
                             {"kind", kind}};
    const auto generationNow = generation_;
    client_->prepareCommand(intent, plaintext,
                            guarded([this, generationNow, callback = std::move(callback)](
                                        const QJsonValue& value, const QString& error) mutable {
                                // Nothing is stored or sent yet, so a fence or an error here is a
                                // plain failure.
                                if (generationNow != generation_) {
                                    callback({}, reason_);
                                    return;
                                }
                                if (!error.isEmpty()) {
                                    callback({}, error);
                                    return;
                                }
                                persistAndDeliver(OutboxRoute::Session, value.toObject(),
                                                  std::move(callback));
                            }));
}

void RelayMachineBackend::persistAndDeliver(OutboxRoute route, const QJsonObject& envelope,
                                            Callback callback) {
    // Written before anything is sent: after a crash, only this ciphertext is ever submitted.
    QString error;
    if (!outbox_.add(route, envelope, &error)) {
        callback({}, error);
        return;
    }
    const auto id = envelope.value("id").toString();
    unsent_.insert(id);
    callbacks_.insert(id, std::move(callback));
    for (const auto& entry : outbox_.pending())
        if (entry.id() == id)
            deliver(entry);
}

void RelayMachineBackend::resume() {
    for (const auto& entry : outbox_.pending())
        deliver(entry);
}

void RelayMachineBackend::deliver(const CommandOutbox::Entry& entry) {
    const auto id = entry.id();
    if (state_ != ConnectionState::Connected || delivering_.contains(id))
        return;
    if (QDateTime::currentMSecsSinceEpoch() - entry.createdAt >
        options_.commandLifetimeMilliseconds) {
        finish(id, {}, QStringLiteral("The command expired before the host answered."));
        return;
    }
    delivering_.insert(id);
    // From here the relay may hold it, even when its answer is lost.
    unsent_.remove(id);
    const auto generation = generation_;
    const auto submitted =
        guarded([this, entry, id, generation](const QJsonValue&, const QString& error) {
            if (generation != generation_)
                return;
            if (error.isEmpty()) {
                poll(entry);
                return;
            }
            delivering_.remove(id);
            // A link failure, or this device signed out: the relay may still hold it.
            if (linkFailure(error) || fatal(error)) {
                failed(error);
                return;
            }
            // The relay refused this envelope for good (scope, grant, idempotency conflict).
            finish(id, {}, error);
        });
    // The same envelope every time: the relay answers a repeat with the command it already has.
    if (entry.route == OutboxRoute::Worktree)
        client_->submitWorktreeCommand(entry.envelope, submitted);
    else
        client_->submit(entry.envelope, submitted);
}

void RelayMachineBackend::poll(const CommandOutbox::Entry& entry) {
    const auto id = entry.id();
    const auto generation = generation_;
    const auto answered =
        guarded([this, entry, id, generation](const QJsonValue& value, const QString& error) {
            if (generation != generation_)
                return;
            if (!error.isEmpty()) {
                delivering_.remove(id);
                if (linkFailure(error) || fatal(error))
                    failed(error);
                else
                    // A result that fails verification never reaches the UI.
                    finish(id, {}, error);
                return;
            }
            const auto command = value.toObject();
            if (command.contains("result")) {
                finish(id, command, {});
                return;
            }
            if (QDateTime::currentMSecsSinceEpoch() - entry.createdAt >
                options_.commandLifetimeMilliseconds) {
                finish(id, {}, QStringLiteral("The command expired before the host answered."));
                return;
            }
            QTimer::singleShot(options_.commandPollMilliseconds, this, [this, entry, generation] {
                if (generation == generation_ && state_ == ConnectionState::Connected)
                    poll(entry);
                else
                    delivering_.remove(entry.id());
            });
        });
    if (entry.route == OutboxRoute::Worktree)
        client_->worktreeCommand(entry.envelope, answered);
    else
        client_->command(entry.envelope, answered);
}

void RelayMachineBackend::finish(const QString& id, const QJsonObject& command,
                                 const QString& error) {
    delivering_.remove(id);
    // A command's result is applied once, however many times it is read back.
    if (outbox_.isCompleted(id) || !outbox_.isPending(id)) {
        callbacks_.remove(id);
        return;
    }
    QString storage;
    if (!outbox_.complete(id, &storage)) {
        // Recorded before it is applied: otherwise a restart would apply it again. It stays
        // pending, with its callback, and is read back on the next connection.
        qWarning("Relay command outbox not updated: %s", qUtf8Printable(storage));
        return;
    }
    unsent_.remove(id);
    const auto result = command.value("result").toObject();
    const auto status = command.value("status").toString();
    const auto failure = !error.isEmpty() ? error
                         : status == QLatin1String("completed")
                             ? QString{}
                             : command.value("resultCode").toString(QStringLiteral("failed"));
    emit commandCompleted(id, result, failure);
    if (const auto callback = callbacks_.take(id))
        callback(result, failure);
}

void RelayMachineBackend::pollEvents() {
    if (pollingEvents_ || state_ != ConnectionState::Connected || watched_.isEmpty())
        return;
    pollingEvents_ = true;
    const auto generation = generation_;
    const auto round = eventsRound_;
    auto remaining = std::make_shared<int>(int(watched_.size()));
    for (auto it = watched_.cbegin(); it != watched_.cend(); ++it) {
        const auto sessionId = it.key();
        const auto row = sessions_.value(sessionId);
        const auto after = it.value();
        // Every request settles its round; a fence starts a new round (fence()).
        const auto settle = [this, remaining, round] {
            if (--*remaining == 0 && round == eventsRound_)
                pollingEvents_ = false;
        };
        if (row.isEmpty()) {
            settle();
            continue;
        }
        client_->events(row, after,
                        guarded([this, sessionId, generation, settle](const QJsonValue& value,
                                                                      const QString& error) {
                            if (generation != generation_) {
                                settle();
                                return;
                            }
                            if (!error.isEmpty()) {
                                failed(error);
                                settle();
                                return;
                            }
                            const auto page = value.toObject();
                            // Authenticated and decrypted by ControlV2Client::events.
                            for (const auto& event : page.value("events").toArray())
                                emit eventReceived(event.toObject());
                            if (watched_.contains(sessionId))
                                watched_.insert(sessionId, page.value("nextCursor").toInteger());
                            settle();
                        }));
    }
}

void RelayMachineBackend::createSession(const QJsonObject& request, Callback callback) {
    if (hold(callback,
             [this, request](Callback again) { createSession(request, std::move(again)); }))
        return;
    const auto repository = request.value("repository_id").toString();
    const auto worktreeId = request.value("worktree_id").toString();
    QJsonObject worktree;
    for (const auto& value : std::as_const(repositories_))
        if (value.toObject().value("repositoryId").toString() == repository)
            for (const auto& item : value.toObject().value("worktrees").toArray())
                if (item.toObject().value("worktreeId").toString() == worktreeId)
                    worktree = item.toObject();
    // Only a worktree the host vouched for (a verified conversation or its signed read).
    if (worktree.isEmpty() || worktree.value("workspaceId").toString().isEmpty() ||
        request.value("prompt").toString().trimmed().isEmpty()) {
        callback({}, QStringLiteral("Choose a verified worktree of this machine and a prompt."));
        return;
    }
    QJsonObject plaintext{{"prompt", request.value("prompt")}};
    for (const auto* key : {"options", "new_worktree"})
        if (request.contains(QLatin1String(key)))
            plaintext.insert(QLatin1String(key), request.value(QLatin1String(key)));
    const QJsonObject intent{
        {"kind", "session.create"},
        {"workspaceId", worktree.value("workspaceId")},
        {"machineId", relayMachineId_},
        {"repositoryId", repository},
        {"worktreeId", worktreeId},
        {"expectedCatalogRevision", worktree.value("catalogRevision").toInteger(0)},
        {"expectedLifecycleRevision", worktree.value("lifecycleRevision").toInteger(0)}};
    const auto generation = generation_;
    client_->prepareWorktreeCommand(
        intent, plaintext,
        guarded([this, generation, callback = std::move(callback)](const QJsonValue& value,
                                                                   const QString& error) mutable {
            if (generation != generation_) {
                callback({}, reason_);
                return;
            }
            if (!error.isEmpty()) {
                callback({}, error);
                return;
            }
            persistAndDeliver(OutboxRoute::Worktree, value.toObject(), std::move(callback));
        }));
}

void RelayMachineBackend::startTurn(const QString& sessionId, qint64 generation,
                                    const QJsonObject& request, Callback callback) {
    sessionCommand(QStringLiteral("session.start_turn"), sessionId, generation, request,
                   std::move(callback));
}

void RelayMachineBackend::steerTurn(const QString& sessionId, qint64 generation,
                                    const QJsonObject& request, Callback callback) {
    sessionCommand(QStringLiteral("session.steer"), sessionId, generation, request,
                   std::move(callback));
}

void RelayMachineBackend::stopTurn(const QString& sessionId, qint64 generation, Callback callback) {
    sessionCommand(QStringLiteral("session.interrupt"), sessionId, generation, {},
                   std::move(callback));
}

void RelayMachineBackend::pendingApprovals(const QString& sessionId, qint64 generation,
                                           const QString& afterId, Callback callback) {
    QJsonObject payload;
    if (!afterId.isEmpty())
        payload.insert("after_id", afterId);
    sessionCommand(QStringLiteral("session.pending_approvals"), sessionId, generation, payload,
                   std::move(callback));
}

void RelayMachineBackend::resolveApproval(const QString& sessionId, qint64 generation,
                                          const QString& approvalId, const QJsonObject& resolution,
                                          Callback callback) {
    sessionCommand(QStringLiteral("approval.resolve"), sessionId, generation,
                   {{"approval_id", approvalId}, {"resolution", resolution}}, std::move(callback));
}

void RelayMachineBackend::answerQuestion(const QString& sessionId, qint64 generation,
                                         const QString& questionId, const QJsonObject& resolution,
                                         Callback callback) {
    // The daemon keys questions by the same approval id as approvals.
    sessionCommand(QStringLiteral("question.answer"), sessionId, generation,
                   {{"approval_id", questionId}, {"resolution", resolution}}, std::move(callback));
}

} // namespace ketplus::remote
