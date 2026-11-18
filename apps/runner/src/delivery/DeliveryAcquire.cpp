#include "delivery/DeliveryRecords.h"
#include "delivery/JobDelivery.h"

#include <QJsonArray>

namespace ketplus::runner::delivery {

namespace {

const QString idle = QStringLiteral("idle");

} // namespace

class JobDelivery::Lease final : public session::JobLease {
  public:
    explicit Lease(JobDelivery& delivery) : m_delivery(delivery) {}
    void start() override { m_delivery.sessionStart(); }
    void finish(const QString& outcome, const QString& reason) override {
        m_delivery.close(false, outcome, reason);
    }
    void release(const QString& reason) override { m_delivery.close(true, {}, reason); }

  private:
    JobDelivery& m_delivery;
};

JobDelivery::JobDelivery(QStringList labels, DeliveryPorts ports)
    : m_labels(std::move(labels)), m_ports(std::move(ports)),
      m_lease(std::make_unique<Lease>(*this)), m_retryBackoff(m_ports.random),
      m_renewBackoff(m_ports.random) {}

JobDelivery::~JobDelivery() = default;

session::JobLease& JobDelivery::lease() { return *m_lease; }

bool JobDelivery::takesJobs() const {
    return !m_draining && !m_disabled && !m_storageBroken && !m_credentialLost;
}

void JobDelivery::heartbeatAnswered(const bool pending, const bool disabled) {
    m_disabled = disabled;
    if (pending)
        acquire({});
}

void JobDelivery::jobAvailable(const QString& jobId) { acquire(jobId); }

void JobDelivery::drain() {
    m_draining = true;
    if (m_state == State::Held && m_sessionOpen && m_ports.session.withdraw()) {
        m_sessionOpen = false;
        close(true, {}, QStringLiteral("draining"));
    }
    // An acquire still waiting for its answer goes on: a lease it brings is released.
}

void JobDelivery::storageFailed(const QString& error) { markStorageBroken(error); }

void JobDelivery::markStorageBroken(const QString& error) {
    if (m_storageBroken)
        return;
    m_storageBroken = true;
    m_ports.report(error);
}

bool JobDelivery::journal(const JournalRecord::Step step) {
    m_record.step = step;
    if (m_ports.journal.write(m_record))
        return true;
    markStorageBroken(QStringLiteral("The job journal could not be written. This runner takes "
                                     "no new job until it is restarted."));
    return false;
}

// One acquire at a time, only while idle and taking jobs. The acquireId reaches the journal
// before the call, so an answer lost to a crash or the network is asked for again.
void JobDelivery::acquire(const QString& jobId) {
    if (m_state != State::Idle || !takesJobs())
        return;
    m_record = JournalRecord{};
    m_record.acquireId = m_ports.newAcquireId();
    m_record.wantedJobId = jobId;
    if (!journal(JournalRecord::Step::Acquiring))
        return;
    m_state = State::Acquiring;
    m_retryBackoff.reset();
    sendAcquire();
}

void JobDelivery::sendAcquire() {
    LeaseCall call;
    call.kind = LeaseCall::Kind::Acquire;
    call.acquireId = m_record.acquireId;
    call.jobId = m_record.wantedJobId;
    m_ports.relay.send(call);
}

void JobDelivery::onAcquire(const CallResult& result) {
    switch (result.kind) {
    case CallResult::Kind::Ok:
        if (const std::optional<HeldLease> lease = leaseFromReply(result.body))
            takeLease(*lease);
        else
            retryLater(); // unreadable: the same acquireId again
        return;
    case CallResult::Kind::Lost:
        retryLater();
        return;
    case CallResult::Kind::CredentialLost:
        if (!m_credentialLost) {
            m_credentialLost = true;
            m_ports.report(QStringLiteral("The Relay refused this runner's credential. It "
                                          "takes no new job; register it again."));
        }
        endLease();
        return;
    case CallResult::Kind::Refused:
        break;
    }
    if (result.refused("runner_busy")) {
        onBusy(result);
        return;
    }
    if (result.refused("rate_limited")) {
        retryLater();
        return;
    }
    if (result.refused("runner_disabled"))
        m_disabled = true; // until a heartbeat says otherwise
    // job_unavailable, not_paired, label_mismatch and the rest: idle until the next trigger.
    endLease();
}

// The Relay says a lease is held already. Without its requester it cannot be checked, so it
// is given back. (An acquire starts only from a journal without a lease, so the journal never
// knows the requester here.)
void JobDelivery::onBusy(const CallResult& result) {
    const std::optional<HeldLease> busy = busyLeaseFromRefusal(result.body);
    if (!busy) {
        retryLater();
        return;
    }
    m_record.lease = *busy;
    m_record.acquireId.clear();
    m_startUnknown = true;
    journal(JournalRecord::Step::Leased);
    m_state = State::Held;
    m_renewing = true;
    m_renewBackoff.reset();
    m_ports.clock.arm(DeliveryTimer::Renew, renewIntervalMs);
    close(true, {}, QStringLiteral("untrusted_requester"));
}

// The lease reaches the journal first; the session sees it only from a pinned desktop, with
// labels this runner has, while it takes jobs.
void JobDelivery::takeLease(const HeldLease& lease) {
    m_ports.clock.disarm(DeliveryTimer::Retry);
    m_record.lease = lease;
    m_record.acquireId.clear();
    m_record.wantedJobId.clear();
    m_startUnknown = false;
    m_leaseGone = false;
    const bool recorded = journal(JournalRecord::Step::Leased);
    m_state = State::Held;
    m_lastPhase = QStringLiteral("assigned");
    m_renewing = true;
    m_renewBackoff.reset();
    m_ports.clock.arm(DeliveryTimer::Renew, renewIntervalMs);
    if (!recorded) {
        close(true, {}, QStringLiteral("draining"));
        return;
    }
    if (const std::optional<QString> reason = releaseReason(lease)) {
        close(true, {}, *reason);
        return;
    }
    openSession(lease);
}

// The session takes a job only while it holds none; one that refuses has its lease given back.
void JobDelivery::openSession(const HeldLease& lease) {
    m_sessionOpen = true;
    if (m_ports.session.open(lease))
        return;
    m_sessionOpen = false;
    if (!m_sessionRefusalReported) {
        m_sessionRefusalReported = true;
        m_ports.report(QStringLiteral("The job session still held another job; the new lease "
                                      "was given back."));
    }
    close(true, {}, QStringLiteral("draining"));
}

std::optional<QString> JobDelivery::releaseReason(const HeldLease& lease) const {
    if (lease.machineId.isEmpty() || !m_ports.trust.trusted(lease.machineId))
        return QStringLiteral("untrusted_requester");
    for (const QString& label : lease.runsOn)
        if (!m_labels.contains(label))
            return QStringLiteral("label_mismatch");
    if (!takesJobs() && !m_disabled)
        return QStringLiteral("draining");
    return std::nullopt;
}

void JobDelivery::restore(const std::optional<JournalRecord>& record) {
    if (!record)
        return;
    m_record = *record;
    switch (record->step) {
    case JournalRecord::Step::Acquiring:
        m_state = State::Acquiring;
        sendAcquire(); // the same acquireId: the same lease if one was granted
        return;
    case JournalRecord::Step::Leased:
        m_state = State::Held;
        m_lastPhase = QStringLiteral("assigned");
        m_renewing = true;
        if (record->lease.machineId.isEmpty()) {
            m_startUnknown = true; // learned through runner_busy
            close(true, {}, QStringLiteral("untrusted_requester"));
        } else if (const std::optional<QString> reason = releaseReason(record->lease)) {
            close(true, {}, *reason);
        } else {
            openSession(record->lease); // its connection window starts again
        }
        renew();
        return;
    case JournalRecord::Step::SpecAccepted:
    case JournalRecord::Step::Started:
    case JournalRecord::Step::Closing: {
        // The spec was taken: the command never runs again.
        m_state = State::Running;
        m_lastPhase =
            record->startedAt.isEmpty() ? QStringLiteral("assigned") : QStringLiteral("running");
        m_renewing = true;
        m_startUnknown = record->lease.machineId.isEmpty(); // learned through runner_busy
        const RestartPolicy::Finish finish = m_ports.restart.afterRestart(*record);
        close(finish.release, finish.outcome, finish.reason);
        renew();
        return;
    }
    }
}

QJsonObject JobDelivery::heartbeatFields() const {
    const bool holding = m_state != State::Idle && m_state != State::Acquiring;
    const QString state = m_draining || m_storageBroken ? QStringLiteral("draining")
                          : holding                     ? QStringLiteral("busy")
                                                        : idle;
    QJsonValue current = QJsonValue::Null;
    if (holding)
        current = QJsonObject{{QStringLiteral("jobId"), m_record.lease.jobId},
                              {QStringLiteral("leaseId"), m_record.lease.leaseId},
                              {QStringLiteral("phase"), phase()}};
    return {{QStringLiteral("state"), state}, {QStringLiteral("currentJob"), current}};
}

QStringList effectiveLabels(const QStringList& userLabels, const QString& arch) {
    QStringList labels = userLabels;
    for (const QString& system : {QStringLiteral("os:linux"), QStringLiteral("arch:") + arch})
        if (!labels.contains(system))
            labels.append(system);
    return labels;
}

} // namespace ketplus::runner::delivery
