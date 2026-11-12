#include "machine/MachineConversationSession.h"

#include "MachineRows.h"

#include <QJsonArray>

#include <memory>
#include <utility>

namespace ketplus {

namespace {

bool endsRequest(const QString& type) {
    return type == QLatin1String("approval.resolved") ||
           type == QLatin1String("approval.expired") ||
           type == QLatin1String("question.answered") || type == QLatin1String("question.expired");
}

} // namespace

MachineConversationSession::MachineConversationSession(remote::MachineBackend* backend,
                                                       QString sessionId, const qint64 generation,
                                                       QObject* parent)
    : QObject(parent), backend_(backend), sessionId_(std::move(sessionId)),
      generation_(generation) {
    if (backend == nullptr)
        return;
    machineId_ = backend->machineId();
    lastState_ = backend->connectionState();
    connect(backend, &remote::MachineBackend::eventReceived, this,
            &MachineConversationSession::applyEvent);
    connect(backend, &remote::MachineBackend::commandCompleted, this,
            &MachineConversationSession::commandCompleted);
    connect(backend, &remote::MachineBackend::connectionStateChanged, this,
            [this](const State state, const QString&) {
                const State previous = lastState_;
                lastState_ = state;
                // Recovery is automatic: what happened while away is read again.
                if (state == State::Connected && previous != State::Connected)
                    load();
                emit changed();
            });
    connect(backend, &QObject::destroyed, this, &MachineConversationSession::changed);
}

remote::MachineBackend::Callback
MachineConversationSession::guarded(std::function<void(const QJsonObject&, const QString&)> done) {
    // An answer after this conversation closed reaches nothing.
    return [self = QPointer<MachineConversationSession>(this),
            done = std::move(done)](const QJsonObject& result, const QString& error) {
        if (self != nullptr)
            done(result, error);
    };
}

void MachineConversationSession::setRunning(const bool running) {
    if (running_ == running)
        return;
    running_ = running;
    emit changed();
}

bool MachineConversationSession::canAct() const {
    return backend_ != nullptr && backend_->connectionState() == State::Connected;
}

QString MachineConversationSession::blockedReason() const {
    if (backend_ == nullptr)
        return tr("This machine is no longer connected.");
    const QString reason = backend_->connectionReason();
    switch (backend_->connectionState()) {
    case State::Connected:
        return {};
    case State::Connecting:
        return tr("Connecting…");
    case State::Reconnecting:
        return reason.isEmpty() ? tr("Reconnecting…") : tr("Reconnecting… %1").arg(reason);
    case State::Offline:
        return reason.isEmpty() ? tr("This machine is offline.") : reason;
    }
    return {};
}

void MachineConversationSession::load() {
    if (backend_ == nullptr)
        return;
    backend_->transcriptWindow(sessionId_, generation_, {{QStringLiteral("turn_limit"), 3}},
                               guarded([this](const QJsonObject& result, const QString& error) {
                                   if (!error.isEmpty()) {
                                       fail(error); // the cached transcript stays as it was
                                       return;
                                   }
                                   lastError_.clear();
                                   transcript_.reset(
                                       result.value(QLatin1String("events")).toArray());
                                   emit changed();
                               }));
    loadApprovals();
}

void MachineConversationSession::loadApprovals() {
    // Pages until the machine says there are no more, then replaces the cards at once. A newer
    // load supersedes one still paging.
    loadApprovalsPage({}, std::make_shared<QVector<QJsonObject>>(), ++approvalsLoad_);
}

void MachineConversationSession::loadApprovalsPage(
    const QString& afterId, const std::shared_ptr<QVector<QJsonObject>>& collected,
    const quint64 load) {
    if (backend_ == nullptr)
        return;
    backend_->pendingApprovals(
        sessionId_, generation_, afterId,
        guarded([this, collected, load](const QJsonObject& result, const QString& error) {
            if (load != approvalsLoad_)
                return;
            if (!error.isEmpty()) {
                fail(error);
                return;
            }
            for (const QJsonValue& value :
                 result.value(QLatin1String("pending_approvals")).toArray())
                collected->append(value.toObject());
            const QString nextId =
                result.value(QLatin1String("pending_approvals_next_id")).toString();
            if (result.value(QLatin1String("pending_approvals_has_more")).toBool() &&
                !nextId.isEmpty()) {
                loadApprovalsPage(nextId, collected, load);
                return;
            }
            approvals_ = *collected;
            emit changed();
        }));
}

void MachineConversationSession::applyEvent(const QJsonObject& event) {
    if (event.value(QLatin1String("session_id")).toString() != sessionId_)
        return;
    if (event.contains(QLatin1String("session_generation")) &&
        event.value(QLatin1String("session_generation")).toInteger() != generation_)
        return;
    const QString type = event.value(QLatin1String("event_type")).toString();
    bool changed = transcript_.apply(event);
    if (type == QLatin1String("turn.started") || type == QLatin1String("user.message")) {
        changed = changed || !running_;
        running_ = true;
    } else if (type == QLatin1String("turn.completed")) {
        changed = changed || running_;
        running_ = false;
        // A turn's pending requests expire with it.
        loadApprovals();
    }
    if (type == QLatin1String("approval.requested") || type == QLatin1String("question.request")) {
        loadApprovals();
    } else if (endsRequest(type)) {
        const QString id = event.value(QLatin1String("payload"))
                               .toObject()
                               .value(QLatin1String("approval_id"))
                               .toString();
        const auto removed = approvals_.removeIf([&id](const QJsonObject& request) {
            return request.value(QLatin1String("approval_id")).toString() == id;
        });
        changed = changed || removed > 0;
    }
    if (changed)
        emit this->changed();
}

void MachineConversationSession::send(const QString& text) {
    const QString prompt = text.trimmed();
    if (prompt.isEmpty() || !canAct())
        return;
    const QJsonObject request{{QStringLiteral("prompt"), prompt}};
    const auto done = write({});
    if (running_)
        backend_->steerTurn(sessionId_, generation_, request, done);
    else
        backend_->startTurn(sessionId_, generation_, request, done);
}

void MachineConversationSession::stop() {
    if (!canAct())
        return;
    backend_->stopTurn(sessionId_, generation_, write({}));
}

void MachineConversationSession::resolveApproval(const QString& approvalId, const bool accept) {
    if (!canAct())
        return;
    backend_->resolveApproval(sessionId_, generation_, approvalId,
                              {{QStringLiteral("decision"),
                                accept ? QStringLiteral("accept") : QStringLiteral("decline")}},
                              write(approvalId));
}

void MachineConversationSession::answerQuestion(const QString& questionId,
                                                const QJsonObject& resolution) {
    if (!canAct())
        return;
    backend_->answerQuestion(sessionId_, generation_, questionId, resolution, write(questionId));
}

remote::MachineBackend::Callback MachineConversationSession::write(const QString& approvalId) {
    // A write the route holds (Connecting, Reconnecting: up to 15 minutes on the Relay) is
    // shown as waiting; one it queued finishes on commandCompleted().
    ++waiting_;
    emit changed();
    return guarded([this, approvalId](const QJsonObject& result, const QString& error) {
        --waiting_;
        const QString commandId = remote::queuedCommandId(result);
        if (error.isEmpty() && !commandId.isEmpty()) {
            queued_.insert(commandId, approvalId);
            emit changed();
            return;
        }
        finishWrite(approvalId, error);
    });
}

void MachineConversationSession::commandCompleted(const QString& commandId, const QJsonObject&,
                                                  const QString& error) {
    const auto it = queued_.constFind(commandId);
    if (it == queued_.cend())
        return;
    const QString approvalId = *it;
    queued_.erase(it);
    finishWrite(approvalId, error);
}

void MachineConversationSession::finishWrite(const QString& approvalId, const QString& error) {
    if (error.isEmpty()) {
        lastError_.clear();
        emit changed();
    } else if (!approvalId.isEmpty()) {
        markExpired(approvalId, error);
    } else {
        fail(error);
    }
}

QString MachineConversationSession::writeStatus() const {
    if (waiting_ > 0 && !canAct())
        return tr("Waiting for the machine to reconnect. What you sent goes through when it "
                  "does (held for up to 15 minutes).");
    if (waiting_ > 0)
        return tr("Sending…");
    if (!queued_.isEmpty())
        return tr("Sent. Waiting for the machine to confirm…");
    return {};
}

void MachineConversationSession::markExpired(const QString& approvalId, const QString& error) {
    // ADR 0119: an answer that fails (expired, offline) marks the card expired.
    for (QJsonObject& request : approvals_)
        if (request.value(QLatin1String("approval_id")).toString() == approvalId) {
            request.insert(QStringLiteral("expired"), true);
            request.insert(QStringLiteral("expired_reason"), error);
        }
    fail(error);
}

void MachineConversationSession::fail(const QString& error) {
    lastError_ = error;
    emit changed();
}

} // namespace ketplus
