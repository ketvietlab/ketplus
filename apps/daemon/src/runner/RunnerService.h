#pragma once

#include "runner/RunnerPairing.h"
#include "runner/RunnerPinStore.h"
#include "runner/RunnerRelayClient.h"
#include "runner/RunnerRelayPort.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

// Runner pairing and management for the local IPC types runner.*. Every operation runs only
// while the Relay offers runner.relay/1; otherwise it answers runner_unavailable and calls no
// runner route. Failures answer {"ok": false, "error_code", "error_message"}.
namespace ketplus::daemon::runner {

class RunnerService final : public QObject {
    Q_OBJECT

  public:
    using Callback = std::function<void(const QJsonObject& result, const QString& error)>;
    using Clock = std::function<QDateTime()>;
    using Scheduler = std::function<void(int delayMs, std::function<void()> task)>;

    static constexpr int firstPollMs = 2'000;
    static constexpr int longestPollMs = 30'000;
    static constexpr int maximumPairings = 16;
    static constexpr int maximumListPages = 20;

    explicit RunnerService(std::unique_ptr<RunnerRelayPort> port, QObject* parent = nullptr);
    ~RunnerService() override;

    [[nodiscard]] static bool handlesType(const QString& type);
    void handle(const QString& type, const QJsonObject& payload, Callback callback);

    // Tests drive time; production uses the UTC clock and QTimer.
    void setTime(Clock clock, Scheduler scheduler);
    [[nodiscard]] RunnerRelayPort& port() { return *port_; }

  signals:
    // {"ticket_id", "state"}: never a code, a token or a secret.
    void pairingChanged(const QJsonObject& payload);
    // The runner is no longer trusted here: removed or unpaired, its pin already gone.
    // `clientId` names the local connection that asked, or is empty; never read from a payload.
    void runnerRemoved(const QString& runnerId, const QString& clientId);

  private:
    using PairingPointer = std::shared_ptr<RunnerPairing>;

    void createPairing(const QJsonObject& payload, Callback callback);
    void pairingStatus(const QJsonObject& payload, Callback callback);
    void approvePairing(const QJsonObject& payload, Callback callback);
    void sendApproval(const PairingPointer& pairing, Callback callback);
    void rejectPairing(const QJsonObject& payload, Callback callback);
    void listRunners(Callback callback);
    void collectRunners(QList<RunnerSummary> runners, const std::optional<QString>& cursor,
                        int page, Callback callback);
    void changeState(const QJsonObject& payload, RunnerRelayClient::StateChange change,
                     Callback callback);
    void unpairRunner(const QJsonObject& payload, Callback callback);

    void schedulePoll(const PairingPointer& pairing, int delayMs);
    void poll(const QString& ticketId, quint64 generation);
    // A pairing that cannot go on: the account changed or runners are no longer offered.
    bool stopIfStale(const PairingPointer& pairing);
    void finishActive(const PairingPointer& pairing, Callback callback);
    void changed(const RunnerPairing& pairing);
    void prunePairings();
    [[nodiscard]] PairingPointer pairing(const QJsonObject& payload) const;

    static void fail(const Callback& callback, const QString& code);
    static void fail(const Callback& callback, const RunnerFailure& failure);

    std::unique_ptr<RunnerRelayPort> port_;
    RunnerRelayClient client_;
    RunnerPinStore pins_;
    Clock clock_;
    Scheduler scheduler_;
    QHash<QString, PairingPointer> pairings_;
    struct PollState {
        int delayMs = firstPollMs;
        quint64 generation = 0;
    };
    QHash<QString, PollState> polls_;
};

} // namespace ketplus::daemon::runner
