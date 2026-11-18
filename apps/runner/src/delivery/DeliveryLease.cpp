#include "delivery/DeliveryRecords.h"
#include "delivery/JobDelivery.h"

namespace ketplus::runner::delivery {

namespace {

bool lostLease(const CallResult& result) {
    return result.refused("lease_expired") || result.refused("job_cancelled");
}

} // namespace

void JobDelivery::answered(const LeaseCall& call, const CallResult& result) {
    if (call.kind == LeaseCall::Kind::Acquire) {
        if (m_state == State::Acquiring && call.acquireId == m_record.acquireId)
            onAcquire(result);
        return;
    }
    if (m_state == State::Idle || call.leaseId != m_record.lease.leaseId)
        return; // a lease that is over
    switch (call.kind) {
    case LeaseCall::Kind::Acquire:
        return;
    case LeaseCall::Kind::Renew:
        onRenew(result);
        return;
    case LeaseCall::Kind::Start:
        if (m_state == State::Starting)
            onStart(result);
        return;
    case LeaseCall::Kind::Finish:
    case LeaseCall::Kind::Release:
        if (m_state == State::Closing)
            onClose(result);
        return;
    }
}

void JobDelivery::timerFired(const DeliveryTimer timer) {
    if (timer == DeliveryTimer::Renew)
        renew();
    else
        retry();
}

// The session took the spec (its accepted-jobs record is on disk): the step is journaled, then
// `job.start` asked.
void JobDelivery::sessionStart() {
    if (m_state != State::Held) {
        m_ports.session.leaseStartRefused(QStringLiteral("lease_expired"));
        return;
    }
    if (!journal(JournalRecord::Step::SpecAccepted)) {
        m_sessionOpen = false;
        m_ports.session.leaseStartRefused(QStringLiteral("lease_lost"));
        close(true, {}, QStringLiteral("draining"));
        return;
    }
    m_state = State::Starting;
    m_retryBackoff.reset();
    LeaseCall call;
    call.kind = LeaseCall::Kind::Start;
    call.leaseId = m_record.lease.leaseId;
    m_ports.relay.send(call);
}

void JobDelivery::onStart(const CallResult& result) {
    if (result.kind == CallResult::Kind::Ok) {
        const std::optional<QString> startedAt = startedAtFromReply(result.body);
        if (!startedAt) {
            retryLater();
            return;
        }
        m_record.startedAt = *startedAt;
        journal(JournalRecord::Step::Started); // started either way: a failure only marks storage
        m_state = State::Running;
        if (m_leaseGone) {
            endLease();
            return;
        }
        m_ports.session.leaseStarted(*startedAt);
        if (m_ports.session.phase().isEmpty()) // the session dropped the job meanwhile
            close(false, QStringLiteral("rejected"), QStringLiteral("lease_lost"));
        return;
    }
    if (result.kind == CallResult::Kind::Lost || result.refused("rate_limited")) {
        retryLater();
        return;
    }
    const QString code = result.kind == CallResult::Kind::CredentialLost
                             ? QStringLiteral("lease_lost")
                             : result.code;
    m_sessionOpen = false;
    m_state = State::Held;
    if (!m_leaseGone)
        m_ports.session.leaseStartRefused(code); // may finish the lease at once
    if (m_state != State::Held)
        return;
    if (m_leaseGone || lostLease(result) || result.kind == CallResult::Kind::CredentialLost)
        endLease();
    else
        close(false, QStringLiteral("rejected"), QStringLiteral("lease_lost"));
}

// Every 60 seconds from the lease until it is closed, also while the session holds a result.
void JobDelivery::renew() {
    if (!m_renewing || m_renewInFlight)
        return;
    m_renewInFlight = true;
    m_lastPhase = phase();
    LeaseCall call;
    call.kind = LeaseCall::Kind::Renew;
    call.leaseId = m_record.lease.leaseId;
    call.phase = m_lastPhase;
    m_ports.relay.send(call);
}

void JobDelivery::onRenew(const CallResult& result) {
    m_renewInFlight = false;
    if (!m_renewing)
        return;
    if (result.kind == CallResult::Kind::Ok) {
        m_renewBackoff.reset();
        m_ports.clock.arm(DeliveryTimer::Renew, renewIntervalMs);
        return;
    }
    const bool ends = lostLease(result) || result.refused("entitlement_required") ||
                      result.refused("unsupported");
    if (!ends) {
        // No answer, a server error, rate_limited or a refusal that says nothing of the lease:
        // the job goes on and the renewal is tried again.
        if (result.kind == CallResult::Kind::CredentialLost && !m_credentialLost) {
            m_credentialLost = true;
            m_ports.report(QStringLiteral("The Relay refused this runner's credential. The "
                                          "current job runs to its end; register the runner "
                                          "again."));
        }
        m_ports.clock.arm(DeliveryTimer::Renew, m_renewBackoff.next());
        return;
    }
    m_renewing = false;
    m_ports.clock.disarm(DeliveryTimer::Renew);
    if (lostLease(result))
        m_leaseGone = true;
    const QString reason =
        result.refused("job_cancelled") ? cancelReasonFromRefusal(result.body) : QString();
    if (!m_sessionOpen)
        return;
    const State before = m_state;
    m_ports.session.leaseLost(result.code, reason);
    if (m_state == before && before == State::Held && m_ports.session.phase().isEmpty()) {
        // The session dropped a job that had no spec, without a finish.
        m_sessionOpen = false;
        endLease();
    }
}

// The one finish or release of this lease; a second one is ignored. It is journaled first, but
// sent even when the journal fails: closing a lease is always safe.
void JobDelivery::close(const bool release, const QString& outcome, const QString& reason) {
    if (m_state == State::Idle || m_state == State::Acquiring || m_state == State::Closing)
        return;
    m_lastPhase = phase();
    m_sessionOpen = false;
    m_record.release = release;
    m_record.outcome = release ? QString() : outcome;
    m_record.reason = reason;
    journal(JournalRecord::Step::Closing);
    m_state = State::Closing;
    m_retryBackoff.reset();
    m_ports.clock.disarm(DeliveryTimer::Retry);
    sendClose();
}

void JobDelivery::sendClose() {
    LeaseCall call;
    call.kind = m_record.release ? LeaseCall::Kind::Release : LeaseCall::Kind::Finish;
    call.leaseId = m_record.lease.leaseId;
    call.outcome = m_record.outcome;
    call.reason = m_record.reason;
    m_ports.relay.send(call);
}

void JobDelivery::onClose(const CallResult& result) {
    if (result.kind == CallResult::Kind::Lost || result.refused("rate_limited")) {
        retryLater(); // the same body again
        return;
    }
    if (result.refused("job_started") && m_record.release && m_startUnknown) {
        // A lease found through runner_busy that had been started: end it as interrupted.
        m_startUnknown = false;
        m_record.release = false;
        m_record.outcome = QStringLiteral("interrupted");
        m_record.reason = QStringLiteral("runner_restarted");
        journal(JournalRecord::Step::Closing);
        m_retryBackoff.reset();
        sendClose();
        return;
    }
    if (result.kind == CallResult::Kind::CredentialLost)
        m_credentialLost = true;
    // Recorded, lease_expired, lease_closed, job_started or another refusal: over.
    endLease();
}

void JobDelivery::retryLater() { m_ports.clock.arm(DeliveryTimer::Retry, m_retryBackoff.next()); }

void JobDelivery::retry() {
    switch (m_state) {
    case State::Acquiring:
        sendAcquire();
        return;
    case State::Starting: {
        LeaseCall call;
        call.kind = LeaseCall::Kind::Start;
        call.leaseId = m_record.lease.leaseId;
        m_ports.relay.send(call);
        return;
    }
    case State::Closing:
        sendClose();
        return;
    case State::Idle:
    case State::Held:
    case State::Running:
        return;
    }
}

void JobDelivery::endLease() {
    m_ports.clock.disarm(DeliveryTimer::Renew);
    m_ports.clock.disarm(DeliveryTimer::Retry);
    if (!m_ports.journal.clear())
        markStorageBroken(QStringLiteral("The job journal could not be cleared. This runner "
                                         "takes no new job until it is restarted."));
    m_record = JournalRecord{};
    m_state = State::Idle;
    m_sessionOpen = false;
    m_renewing = false;
    m_renewInFlight = false;
    m_startUnknown = false;
    m_leaseGone = false;
    m_lastPhase = QStringLiteral("assigned");
}

QString JobDelivery::phase() const {
    if (m_sessionOpen) {
        const QString current = m_ports.session.phase();
        if (!current.isEmpty())
            return current;
    }
    return m_lastPhase;
}

} // namespace ketplus::runner::delivery
