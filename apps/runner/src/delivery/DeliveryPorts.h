#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

// What job delivery needs from the rest of the runner. Like the job session, it opens no socket
// and reads no clock: every effect goes through these interfaces and every answer comes back as
// a call on JobDelivery.
namespace ketplus::runner::delivery {

// A lease the Relay granted, with what the session and a restart need to know about it.
struct HeldLease {
    QString jobId;
    qint64 attempt = 0;
    QString leaseId;
    QString leaseExpiresAt;
    QString machineId;   // requester.machineId
    QString workspaceId; // requester.workspaceId
    QStringList runsOn;
};

// One call on the Relay's job routes.
struct LeaseCall {
    enum class Kind { Acquire, Renew, Start, Finish, Release };
    Kind kind = Kind::Acquire;
    QString acquireId; // Acquire
    QString jobId;     // Acquire, may be empty
    QString leaseId;   // every other kind
    QString phase;     // Renew: "assigned", "syncing" or "running"
    QString outcome;   // Finish
    QString reason;    // Finish and Release
};

struct CallResult {
    enum class Kind {
        Ok,
        Refused,        // a refusal code and its body, after one token renewal on invalid_access
        Lost,           // network failure, 5xx or an unreadable success: the call may have happened
        CredentialLost, // no token can be had any more: the refresh credential was refused
    };
    Kind kind = Kind::Lost;
    QString code;
    QJsonObject body;

    [[nodiscard]] bool refused(const char* refusal) const {
        return kind == Kind::Refused && code == QLatin1StringView(refusal);
    }
};

// The Relay's job routes. Answer: JobDelivery::answered, never from inside send().
class RelayCalls {
  public:
    virtual ~RelayCalls() = default;
    virtual void send(const LeaseCall& call) = 0;
};

// The job's session. Answers come through the session::JobLease that JobDelivery::lease()
// returns.
class SessionLink {
  public:
    virtual ~SessionLink() = default;
    // A new job for the session, which then connects to `machineId`.
    virtual void open(const HeldLease& lease) = 0;
    // Drops the job if no spec came yet; false otherwise.
    [[nodiscard]] virtual bool withdraw() = 0;
    virtual void leaseStarted(const QString& startedAt) = 0;
    virtual void leaseStartRefused(const QString& code) = 0;
    virtual void leaseLost(const QString& code, const QString& reason) = 0;
    // "assigned", "syncing" or "running" while the session holds the job, otherwise empty.
    [[nodiscard]] virtual QString phase() const = 0;
};

// The record of the current job, written before each step it names. Never holds a token, a
// command, an environment or output.
struct JournalRecord {
    enum class Step { Acquiring, Leased, SpecAccepted, Started, Closing };
    Step step = Step::Acquiring;
    QString acquireId;   // Acquiring
    QString wantedJobId; // Acquiring: the jobId asked for, if any
    HeldLease lease;     // from Leased on
    QString startedAt;   // from Started on
    // Closing: the one finish or release sent for this lease.
    bool release = false;
    QString outcome; // finish
    QString reason;
};

class DeliveryJournal {
  public:
    virtual ~DeliveryJournal() = default;
    // Durably, before the step's effect. False when the disk did not take it.
    [[nodiscard]] virtual bool write(const JournalRecord& record) = 0;
    // No job any more. False when the disk did not take it.
    [[nodiscard]] virtual bool clear() = 0;
};

// Whether a desktop may send this runner a job: an active local pin for it.
class RequesterTrust {
  public:
    virtual ~RequesterTrust() = default;
    [[nodiscard]] virtual bool trusted(const QString& machineId) const = 0;
};

// What a restarted runner does with a job whose spec it had taken: the finish to send.
class RestartPolicy {
  public:
    struct Finish {
        bool release = false;
        QString outcome;
        QString reason;
    };
    virtual ~RestartPolicy() = default;
    [[nodiscard]] virtual Finish afterRestart(const JournalRecord& record) const = 0;
};

// Until an executor can be reconciled: never run again. A close that was being sent goes again
// unchanged; any other job ends interrupted (runner_restarted).
class NeverRelaunch final : public RestartPolicy {
  public:
    [[nodiscard]] Finish afterRestart(const JournalRecord& record) const override;
};

enum class DeliveryTimer {
    Renew, // the next renewal: 60 seconds after the last one, sooner after a failure
    Retry, // the next attempt at an acquire, start, finish or release that got no answer
};

class DeliveryClock {
  public:
    virtual ~DeliveryClock() = default;
    virtual void arm(DeliveryTimer timer, qint64 delayMs) = 0;
    virtual void disarm(DeliveryTimer timer) = 0;
};

} // namespace ketplus::runner::delivery
