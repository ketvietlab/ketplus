#include "runner/RunnerService.h"

#include "pairing/PairingIdentity.h"

#include <QJsonArray>
#include <QSet>
#include <QTimer>

namespace ketplus::daemon::runner {
namespace {

const QSet<QString>& types() {
    static const QSet<QString> names{QStringLiteral("runner.pairing.create"),
                                     QStringLiteral("runner.pairing.status"),
                                     QStringLiteral("runner.pairing.approve"),
                                     QStringLiteral("runner.pairing.reject"),
                                     QStringLiteral("runner.list"),
                                     QStringLiteral("runner.disable"),
                                     QStringLiteral("runner.enable"),
                                     QStringLiteral("runner.remove"),
                                     QStringLiteral("runner.unpair")};
    return names;
}

QJsonObject runnerJson(const RunnerSummary& runner, bool pinned) {
    return {{QStringLiteral("runner_id"), runner.runnerId},
            {QStringLiteral("name"), runner.name},
            {QStringLiteral("os"), runner.os},
            {QStringLiteral("arch"), runner.arch},
            {QStringLiteral("labels"), QJsonArray::fromStringList(runner.labels)},
            {QStringLiteral("state"), runner.state},
            {QStringLiteral("runner_version"), runner.runnerVersion},
            {QStringLiteral("last_seen_at"),
             runner.lastSeenAt ? QJsonValue(runner.lastSeenAt->toString(Qt::ISODateWithMs))
                               : QJsonValue()},
            {QStringLiteral("paired_with_this_machine"), runner.pairedWithThisMachine},
            {QStringLiteral("pinned"), pinned},
            // Jobs go only to a runner this machine pinned and the Relay pairs with it.
            {QStringLiteral("trusted"), pinned && runner.pairedWithThisMachine}};
}

} // namespace

RunnerService::RunnerService(std::unique_ptr<RunnerRelayPort> port, QObject* parent)
    : QObject(parent), port_(std::move(port)), client_(*port_), pins_(port_->credentials()),
      clock_([] { return QDateTime::currentDateTimeUtc(); }),
      scheduler_([this](const int delayMs, std::function<void()> task) {
          QTimer::singleShot(delayMs, this, std::move(task));
      }) {}

RunnerService::~RunnerService() = default;

bool RunnerService::handlesType(const QString& type) { return types().contains(type); }

void RunnerService::setTime(Clock clock, Scheduler scheduler) {
    clock_ = std::move(clock);
    scheduler_ = std::move(scheduler);
}

void RunnerService::handle(const QString& type, const QJsonObject& payload, Callback callback,
                           const QString& clientId) {
    if (!port_->runnerRelayAvailable()) {
        fail(callback, QStringLiteral("runner_unavailable"));
        return;
    }
    if (type == QStringLiteral("runner.pairing.create"))
        createPairing(payload, std::move(callback));
    else if (type == QStringLiteral("runner.pairing.status"))
        pairingStatus(payload, std::move(callback));
    else if (type == QStringLiteral("runner.pairing.approve"))
        approvePairing(payload, std::move(callback));
    else if (type == QStringLiteral("runner.pairing.reject"))
        rejectPairing(payload, std::move(callback));
    else if (type == QStringLiteral("runner.list"))
        listRunners(std::move(callback));
    else if (type == QStringLiteral("runner.disable"))
        changeState(payload, RunnerRelayClient::StateChange::Disable, std::move(callback),
                    clientId);
    else if (type == QStringLiteral("runner.enable"))
        changeState(payload, RunnerRelayClient::StateChange::Enable, std::move(callback), clientId);
    else if (type == QStringLiteral("runner.remove"))
        changeState(payload, RunnerRelayClient::StateChange::Remove, std::move(callback), clientId);
    else if (type == QStringLiteral("runner.unpair"))
        unpairRunner(payload, std::move(callback), clientId);
    else
        fail(callback, QStringLiteral("invalid_request"));
}

void RunnerService::listRunners(Callback callback) {
    collectRunners({}, std::nullopt, 0, std::move(callback));
}

void RunnerService::collectRunners(QList<RunnerSummary> runners,
                                   const std::optional<QString>& cursor, const int page,
                                   Callback callback) {
    client_.listRunners(
        cursor, [this, runners = std::move(runners), page,
                 callback = std::move(callback)](const RunnerOutcome<RunnerPage>& outcome) mutable {
            if (!outcome.ok()) {
                fail(callback, outcome.failure);
                return;
            }
            runners.append(outcome.value->runners);
            if (outcome.value->nextCursor && page + 1 < maximumListPages) {
                collectRunners(std::move(runners), outcome.value->nextCursor, page + 1,
                               std::move(callback));
                return;
            }
            const RunnerPinStore::Loaded loaded = pins_.load();
            QJsonArray list;
            QSet<QString> listed;
            for (const RunnerSummary& runner : runners) {
                if (listed.contains(runner.runnerId))
                    continue;
                listed.insert(runner.runnerId);
                list.append(runnerJson(runner, loaded.pins.contains(runner.runnerId)));
            }
            QJsonArray unlisted;
            for (auto it = loaded.pins.cbegin(); it != loaded.pins.cend(); ++it) {
                if (!listed.contains(it.key()))
                    unlisted.append(it.key());
            }
            QJsonObject result{{QStringLiteral("ok"), true},
                               {QStringLiteral("runners"), list},
                               {QStringLiteral("unlisted_pins"), unlisted},
                               {QStringLiteral("complete"), !outcome.value->nextCursor}};
            if (!loaded.error.isEmpty() || !loaded.ignored.isEmpty())
                result.insert(QStringLiteral("pin_error"),
                              loaded.error.isEmpty()
                                  ? QStringLiteral("Some runner pins did not read back and are "
                                                   "not trusted.")
                                  : loaded.error);
            callback(result, {});
        });
}

void RunnerService::changeState(const QJsonObject& payload,
                                const RunnerRelayClient::StateChange change, Callback callback,
                                const QString& clientId) {
    const QString runnerId = payload.value(QStringLiteral("runner_id")).toString();
    if (!isRunnerId(runnerId)) {
        fail(callback, QStringLiteral("invalid_request"));
        return;
    }
    const bool removing = change == RunnerRelayClient::StateChange::Remove;
    // Trust ends before the Relay is asked: a failed call leaves no pin behind.
    if (removing && !pins_.remove(runnerId)) {
        fail(callback, QStringLiteral("pin_store_failed"));
        return;
    }
    if (removing)
        emit runnerRemoved(runnerId, clientId);
    client_.changeState(
        runnerId, change,
        [runnerId, removing,
         callback = std::move(callback)](const RunnerOutcome<QString>& outcome) {
            // A retry after the Relay removed it is done.
            const bool gone = removing && !outcome.ok() &&
                              outcome.failure.code == QStringLiteral("runner_not_found");
            if (!outcome.ok() && !gone) {
                fail(callback, outcome.failure);
                return;
            }
            callback({{QStringLiteral("ok"), true},
                      {QStringLiteral("runner_id"), runnerId},
                      {QStringLiteral("state"), gone ? QStringLiteral("removed") : *outcome.value}},
                     {});
        });
}

void RunnerService::unpairRunner(const QJsonObject& payload, Callback callback,
                                 const QString& clientId) {
    const QString runnerId = payload.value(QStringLiteral("runner_id")).toString();
    if (!isRunnerId(runnerId)) {
        fail(callback, QStringLiteral("invalid_request"));
        return;
    }
    if (!pins_.remove(runnerId)) {
        fail(callback, QStringLiteral("pin_store_failed"));
        return;
    }
    emit runnerRemoved(runnerId, clientId);
    client_.unpair(runnerId,
                   [runnerId, callback = std::move(callback)](const RunnerOutcome<bool>& outcome) {
                       // A retry after the Relay dropped the pair is done.
                       if (!outcome.ok() && outcome.failure.code != QStringLiteral("not_paired")) {
                           fail(callback, outcome.failure);
                           return;
                       }
                       callback({{QStringLiteral("ok"), true},
                                 {QStringLiteral("runner_id"), runnerId},
                                 {QStringLiteral("paired"), false}},
                                {});
                   });
}

void RunnerService::fail(const Callback& callback, const QString& code) {
    fail(callback, runnerFailure(code));
}

void RunnerService::fail(const Callback& callback, const RunnerFailure& failure) {
    callback({{QStringLiteral("ok"), false},
              {QStringLiteral("error_code"), failure.code},
              {QStringLiteral("error_message"), failure.message}},
             {});
}

void RunnerService::changed(const RunnerPairing& pairing) {
    emit pairingChanged({{QStringLiteral("ticket_id"), pairing.ticketId()},
                         {QStringLiteral("state"), RunnerPairing::stateName(pairing.state())}});
}

void RunnerService::prunePairings() {
    // Finished pairings go first, oldest expiry first; one still open is never dropped.
    while (pairings_.size() >= maximumPairings) {
        QString oldest;
        for (auto it = pairings_.cbegin(); it != pairings_.cend(); ++it) {
            if (it.value()->terminal() &&
                (oldest.isEmpty() || it.value()->expiresAt() < pairings_[oldest]->expiresAt()))
                oldest = it.key();
        }
        if (oldest.isEmpty())
            return;
        pairings_.remove(oldest);
        polls_.remove(oldest);
    }
}

RunnerService::PairingPointer RunnerService::pairing(const QJsonObject& payload) const {
    return pairings_.value(payload.value(QStringLiteral("ticket_id")).toString());
}

} // namespace ketplus::daemon::runner
