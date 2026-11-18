#pragma once

#include "delivery/DeliveryPorts.h"
#include "run/Backoff.h"
#include "session/SessionPorts.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

// The runner's side of job delivery: acquires a job from the Relay, checks who sent it, holds
// and renews the lease, starts and closes it for the session, and journals each step first so
// a restarted runner never runs a command twice. One lease at a time.
namespace ketplus::runner::delivery {

inline constexpr qint64 renewIntervalMs = 60'000;

struct DeliveryPorts {
    RelayCalls& relay;
    SessionLink& session;
    DeliveryJournal& journal;
    const RequesterTrust& trust;
    const RestartPolicy& restart;
    DeliveryClock& clock;
    std::function<double()> random;             // [0, 1), for the retry jitter
    std::function<QString()> newAcquireId;      // [A-Za-z0-9_-]{1,128}, unique
    std::function<void(const QString&)> report; // a problem worth one message; never secrets
};

class JobDelivery {
  public:
    enum class State {
        Idle,
        Acquiring, // an acquire waits for its answer
        Held,      // a lease, no spec yet
        Starting,  // `job.start` waits for its answer
        Running,   // started
        Closing,   // the one finish or release waits for its answer
    };

    // `labels`: every label the runner has, user and system.
    JobDelivery(QStringList labels, DeliveryPorts ports);
    ~JobDelivery();
    JobDelivery(const JobDelivery&) = delete;
    JobDelivery& operator=(const JobDelivery&) = delete;

    // The lease interface for the job session.
    [[nodiscard]] session::JobLease& lease();

    // At start: the journal as found. Nothing: no job was held.
    void restore(const std::optional<JournalRecord>& record);
    // The journal or the accepted-jobs record cannot be read or written: no job is taken any
    // more, and the heartbeat says draining, until the process restarts.
    void storageFailed(const QString& error);

    // Triggers.
    void heartbeatAnswered(bool pending, bool disabled);
    void jobAvailable(const QString& jobId);
    // No new job; a lease without a spec is released, a started job runs to its end.
    void drain();

    void answered(const LeaseCall& call, const CallResult& result);
    void timerFired(DeliveryTimer timer);

    // `state` and `currentJob` of the heartbeat.
    [[nodiscard]] QJsonObject heartbeatFields() const;
    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] bool takesJobs() const;

  private:
    class Lease;
    friend class Lease;

    // Acquire (DeliveryAcquire.cpp).
    void acquire(const QString& jobId);
    void sendAcquire();
    void onAcquire(const CallResult& result);
    void onBusy(const CallResult& result);
    void takeLease(const HeldLease& lease);
    [[nodiscard]] std::optional<QString> releaseReason(const HeldLease& lease) const;

    // The lease (DeliveryLease.cpp).
    void sessionStart();
    void onStart(const CallResult& result);
    void renew();
    void onRenew(const CallResult& result);
    void close(bool release, const QString& outcome, const QString& reason);
    void sendClose();
    void onClose(const CallResult& result);
    void retryLater();
    void retry();
    void endLease();
    [[nodiscard]] QString phase() const;
    bool journal(JournalRecord::Step step);
    void markStorageBroken(const QString& error);

    QStringList m_labels;
    DeliveryPorts m_ports;
    std::unique_ptr<Lease> m_lease;
    app::Backoff m_retryBackoff;
    app::Backoff m_renewBackoff;

    State m_state = State::Idle;
    JournalRecord m_record;
    bool m_sessionOpen = false; // the session holds this lease's job
    bool m_renewing = false;    // renewals continue
    bool m_renewInFlight = false;
    bool m_startUnknown = false; // a lease found through runner_busy: maybe started
    bool m_leaseGone = false;    // the Relay ended the lease while a start was asked
    QString m_lastPhase = QStringLiteral("assigned");

    bool m_draining = false;
    bool m_disabled = false;
    bool m_storageBroken = false;
    bool m_credentialLost = false;
};

// Every label of this runner: its user labels, os:linux and arch:<arch>.
[[nodiscard]] QStringList effectiveLabels(const QStringList& userLabels, const QString& arch);

} // namespace ketplus::runner::delivery
