#pragma once

#include "runner/link/LinkPorts.h"
#include "runner/link/OfferCheck.h"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>

#include <memory>

// The desktop's peer connections to runners. It answers runner offers relayed by the Relay,
// never starts a connection, and carries the three channels of each connection for the job
// side. One pending attempt and one open connection per runner.
namespace ketplus::daemon::runner::link {

// An attempt must open its three channels within this time of the job side taking its offer.
inline constexpr qint64 attemptDeadlineMs = 20'000;
inline constexpr int pendingAttemptLimit = 64;
inline constexpr int openConnectionLimit = 64;
// One transfer carries 1 byte to this much.
inline constexpr qsizetype transferSizeLimit = 4 * 1024 * 1024;
// bulkWritable() answers false while this much waits on `bulk`.
inline constexpr quint64 bulkWritableLimit = 4 * 1024 * 1024;
// A transfer that would leave more than this waiting on `bulk` (headers aside) is not sent; the
// connection closes as lost with `bulk_overflow`.
inline constexpr quint64 bulkBufferLimit = 4 * bulkWritableLimit;

class RunnerLinks final : public peer::PeerLinks {
  public:
    RunnerLinks(SignalOut& out, LinkClock& clock, LinkTransportFactory& transports,
                const MachineIdentity& identity, const RunnerPins& pins, LinkEvents& events);
    ~RunnerLinks() override;
    RunnerLinks(const RunnerLinks&) = delete;
    RunnerLinks& operator=(const RunnerLinks&) = delete;

    // A `signal.*` message for these links: a runner offer (isRunnerOffer), or a candidate,
    // end or error of a connection id they own. Anything else is ignored.
    void signal(const QJsonObject& message);
    // True for a pending attempt or an open connection; false once it ended.
    [[nodiscard]] bool owns(const QString& connectionId) const;
    void timerFired(const QString& connectionId);

    // One frame on `control` or `events` of an open connection. A frame above 64 KiB is not
    // sent and the connection stays.
    void send(const QString& connectionId, FrameChannel channel,
              const QByteArray& message) override;
    // One transfer of 1 byte to 4 MiB on `bulk`; `transferId` is 16 lowercase hex characters,
    // not all zero. A transfer the caller got wrong is not sent and the connection stays.
    void sendBulk(const QString& connectionId, const QString& transferId,
                  const QByteArray& bytes) override;
    [[nodiscard]] bool bulkWritable(const QString& connectionId) const override;
    // Ends the attempt or connection without reporting it. The reason is only logged.
    void close(const QString& connectionId, const QString& reason) override;

    class Link;

  private:
    friend class Link;

    void offer(const QJsonObject& message);
    void remoteCandidate(Link& link, const QJsonObject& message);
    void remoteEnd(const QString& connectionId, const QJsonValue& code);

    void localDescription(Link& link, const QString& sdp);
    void localCandidate(Link& link, const QString& candidate, const QString& mid);
    void opened(Link& link);
    void received(Link& link, TransportChannel channel, const QByteArray& bytes, bool binary);
    void writable(Link& link);
    void transportClosed(Link& link);

    // Ends an attempt before its channels opened: signal.end when `tellRelay`, the transport
    // closed, the job side told when it took the offer.
    void endAttempt(const QString& connectionId, const QString& reason, bool tellRelay = true);
    // Closes an open connection and reports it lost.
    void fail(const QString& connectionId, const QString& reason);
    // Removes a connection: deadline disarmed, transport closed, no report.
    std::shared_ptr<Link> drop(const QString& connectionId);
    [[nodiscard]] Link* find(const QString& connectionId) const;
    [[nodiscard]] int count(bool open) const;
    void sendEnd(const QString& connectionId, const QString& reason);

    SignalOut& m_out;
    LinkClock& m_clock;
    LinkTransportFactory& m_transports;
    const MachineIdentity& m_identity;
    const RunnerPins& m_pins;
    LinkEvents& m_events;
    UsedOffers m_used;
    QHash<QString, std::shared_ptr<Link>> m_links;
};

} // namespace ketplus::daemon::runner::link
