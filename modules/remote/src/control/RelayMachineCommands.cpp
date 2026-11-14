#include "control/ControlV2Client.h"
#include "control/RelayMachineBackend.h"
#include <QDateTime>
#include <QTimer>
#include <QUuid>
#include <algorithm>

// Commands of the Relay backend: every command that changes host state goes through the durable
// outbox, and its result is applied once, after the host signature is verified. Reads sent as
// commands (pendingApprovals) are not stored (RelayMachineBackend.cpp holds the other reads).
namespace ketplus::remote {
namespace {
// A decrypted Relay event in the local daemon's session.event shape (LocalIpcServer::
// broadcastEvent), so every MachineBackend consumer reads one shape whatever the route. The
// payload and its routing fields are host-signed; sent_at is the relay's occurredAt, which the
// host does not sign, so it is only a display hint.
QJsonObject localEvent(const QJsonObject& relay) {
    return {{QStringLiteral("protocol_version"), 0},
            {QStringLiteral("message_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("message_type"), QStringLiteral("session.event")},
            {QStringLiteral("session_id"), relay.value("sessionId")},
            {QStringLiteral("session_generation"), relay.value("sessionGeneration")},
            {QStringLiteral("cursor"), relay.value("cursor")},
            {QStringLiteral("turn_sequence"), relay.value("turnSequence")},
            {QStringLiteral("sent_at"), relay.value("occurredAt").isString()
                                            ? relay.value("occurredAt")
                                            : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("payload"),
             QJsonObject{{QStringLiteral("event_type"), relay.value("eventType")},
                         {QStringLiteral("data"), relay.value("payload")}}}};
}

// Errors that leave a command in the outbox for the next connection. A refusal without a code
// ("Control refused the request.") belongs to the command, not to the link.
bool linkFailure(const QString& error) {
    return error == QLatin1String("Control transport failed.") ||
           error == QLatin1String("Invalid or oversized Control response.") ||
           error == QLatin1String("Credential refresh failed.") ||
           error == QLatin1String("Account changed while the request was pending.") ||
           error == QLatin1String("Account changed.") || error == QLatin1String("invalid_access") ||
           error == QLatin1String("Sign in before connecting to a remote host.");
}
} // namespace

void RelayMachineBackend::sessionCommand(const QString& kind, const QString& sessionId,
                                         qint64 generation, const QJsonObject& plaintext,
                                         Callback callback, bool durable) {
    if (hold(callback, [this, kind, sessionId, generation, plaintext, durable](Callback again) {
            sessionCommand(kind, sessionId, generation, plaintext, std::move(again), durable);
        }))
        return;
    const auto row = sessionRow(sessionId, generation);
    if (row.isEmpty()) {
        callback({}, QStringLiteral("This conversation is not in the verified list."));
        return;
    }
    // A write follows its conversation's events; a read (pendingApprovals) does not, so it never
    // starts polling again for a conversation the UI has closed.
    if (prepared_.contains(sessionId) && kind != "session.prepare.cancel" &&
        kind != "session.start_turn" && !kind.startsWith("attachment.stage.")) {
        callback({}, QStringLiteral("This attachment draft is not an active conversation."));
        return;
    }
    if (durable && !prepared_.contains(sessionId) && !watched_.contains(sessionId))
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
                            guarded([this, generationNow, durable, callback = std::move(callback)](
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
                                if (!durable) {
                                    submitRead(value.toObject(), std::move(callback));
                                    return;
                                }
                                persistAndDeliver(OutboxRoute::Session, value.toObject(),
                                                  std::move(callback));
                            }));
}

void RelayMachineBackend::submitRead(const QJsonObject& envelope, Callback callback) {
    // A read changes nothing on the host: it is never written to the outbox, and a lost one is
    // simply asked again by its caller.
    const auto generation = generation_;
    client_->submit(envelope, guarded([this, envelope, generation, callback = std::move(callback)](
                                          const QJsonValue&, const QString& error) mutable {
                        if (generation != generation_) {
                            callback({}, reason_);
                            return;
                        }
                        if (!error.isEmpty()) {
                            if (linkFailure(error) || fatal(error))
                                failed(error);
                            callback({}, error);
                            return;
                        }
                        pollRead(envelope,
                                 QDateTime::currentMSecsSinceEpoch() +
                                     options_.readTimeoutMilliseconds,
                                 std::move(callback));
                    }));
}

void RelayMachineBackend::pollRead(const QJsonObject& envelope, qint64 deadline,
                                   Callback callback) {
    const auto generation = generation_;
    client_->command(
        envelope, guarded([this, envelope, deadline, generation, callback = std::move(callback)](
                              const QJsonValue& value, const QString& error) mutable {
            if (generation != generation_) {
                callback({}, reason_);
                return;
            }
            if (!error.isEmpty()) {
                if (linkFailure(error) || fatal(error))
                    failed(error);
                callback({}, error);
                return;
            }
            const auto command = value.toObject();
            if (command.contains("result")) {
                const auto failure =
                    command.value("status").toString() == QLatin1String("completed")
                        ? QString{}
                        : command.value("resultCode").toString(QStringLiteral("failed"));
                callback(command.value("result").toObject(), failure);
                return;
            }
            if (QDateTime::currentMSecsSinceEpoch() >= deadline) {
                callback({}, QStringLiteral("The host did not answer in time."));
                return;
            }
            QTimer::singleShot(
                options_.commandPollMilliseconds, this,
                [this, envelope, deadline, generation, callback = std::move(callback)]() mutable {
                    if (generation != generation_) {
                        callback({}, reason_);
                        return;
                    }
                    pollRead(envelope, deadline, std::move(callback));
                });
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
    if (recoveredPreparations_.contains(id) && error.isEmpty() &&
        command.value("status") == "completed" &&
        command.value("result").toObject().value("ok").toBool() &&
        !command.value("result").toObject().value("session_id").toString().isEmpty() &&
        command.value("result").toObject().value("session_generation").toInteger() == 1 &&
        command.value("result").toObject().value("expires_at_ms").toInteger() >
            QDateTime::currentMSecsSinceEpoch()) {
        if (!recoveringPreparations_.contains(id))
            discardRecoveredPreparation(id, command);
        return;
    }
    delivering_.remove(id);
    // A command's result is applied once, however many times it is read back.
    if (outbox_.isCompleted(id) || !outbox_.isPending(id)) {
        callbacks_.remove(id);
        recordAttempts_.remove(id);
        return;
    }
    QString storage;
    if (!outbox_.complete(id, &storage)) {
        // Recorded before it is applied: otherwise a restart would apply it again. It stays
        // pending, with its callback, and is read back again while the link stays up (and on
        // the next connection).
        // Backs off (doubling to 30 s) and warns once while the file stays unwritable.
        const auto attempt = recordAttempts_.value(id);
        if (attempt == 0)
            qWarning("Relay command outbox not updated: %s", qUtf8Printable(storage));
        recordAttempts_.insert(id, attempt + 1);
        const auto delay = std::min<qint64>(
            qint64(options_.recordRetryMilliseconds) << std::min(attempt, 15), 30000);
        const auto generation = generation_;
        QTimer::singleShot(delay, this, [this, id, generation] {
            if (generation != generation_ || state_ != ConnectionState::Connected ||
                delivering_.contains(id))
                return;
            for (const auto& entry : outbox_.pending())
                if (entry.id() == id) {
                    delivering_.insert(id);
                    poll(entry);
                }
        });
        return;
    }
    recordAttempts_.remove(id);
    unsent_.remove(id);
    const auto result = command.value("result").toObject();
    const auto status = command.value("status").toString();
    const auto failure = !error.isEmpty() ? error
                         : status == QLatin1String("completed")
                             ? QString{}
                             : command.value("resultCode").toString(QStringLiteral("failed"));
    if (failure.isEmpty()) {
        const auto session = command.value("sessionId").toString();
        const auto kind = command.value("kind").toString();
        if (kind == "session.prepare" && result.value("ok").toBool() &&
            result.value("session_generation").toInteger() == 1 &&
            result.value("expires_at_ms").toInteger() > QDateTime::currentMSecsSinceEpoch()) {
            const auto preparedId = result.value("session_id").toString();
            if (!preparedId.isEmpty())
                prepared_.insert(preparedId, {{"id", preparedId},
                                              {"sessionId", preparedId},
                                              {"workspaceId", command.value("workspaceId")},
                                              {"machineId", relayMachineId_},
                                              {"generation", 1},
                                              {"expires_at_ms", result.value("expires_at_ms")}});
        }
        if (kind == "session.prepare.cancel" && result.value("ok").toBool())
            prepared_.remove(session);
        if (kind == "session.start_turn" && prepared_.contains(session) &&
            result.value("ok").toBool()) {
            sessions_.insert(session, prepared_.take(session));
            watched_.insert(session, 0);
        }
    }
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
                                emit eventReceived(localEvent(event.toObject()));
                            if (watched_.contains(sessionId))
                                watched_.insert(sessionId, page.value("nextCursor").toInteger());
                            settle();
                        }));
    }
}

void RelayMachineBackend::attachmentCommand(const AttachmentOperation operation,
                                            const QString& sessionId, const qint64 generation,
                                            const QJsonObject& request, Callback callback) {
    sessionCommand(attachmentCommandKind(operation), sessionId, generation, request,
                   std::move(callback));
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
    // A read: it changes nothing on the host, so it is not stored in the outbox.
    sessionCommand(QStringLiteral("session.pending_approvals"), sessionId, generation, payload,
                   std::move(callback), false);
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

bool RelayMachineBackend::supports(const Feature feature) const {
    return state_ == ConnectionState::Connected &&
           (feature == Feature::Rename || feature == Feature::Fork ||
            feature == Feature::Attachments ||
            (feature == Feature::SessionPreparation && preparationSupported_));
}

QString RelayMachineBackend::unsupportedReason(const Feature feature) const {
    if (state_ == ConnectionState::Connected && feature == Feature::Archive)
        return tr("Not available through the Relay yet.");
    return MachineBackend::unsupportedReason(feature);
}

void RelayMachineBackend::renameConversation(const QString& sessionId, qint64 generation,
                                             const QString& title, Callback callback) {
    sessionCommand(QStringLiteral("session.rename"), sessionId, generation, {{"title", title}},
                   std::move(callback));
}

void RelayMachineBackend::forkConversation(const QString& sessionId, qint64 generation,
                                           const QString& forkSessionId, Callback callback) {
    // The host forks for cloud clients (CloudControlCommands' remote operations) and answers
    // with the new session in the command's completion.
    sessionCommand(QStringLiteral("session.fork"), sessionId, generation,
                   {{"fork_session_id", forkSessionId}}, std::move(callback));
}

void RelayMachineBackend::setConversationArchived(const QString&, qint64, bool, Callback callback) {
    callback({}, unsupportedReason(Feature::Archive));
}

} // namespace ketplus::remote
