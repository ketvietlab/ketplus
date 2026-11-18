#pragma once

#include "protocol/Frames.h"
#include "runner/peer/JobPeerSession.h"

#include <QList>
#include <QMap>
#include <QStringList>
#include <QTimeZone>

#include <memory>
#include <optional>
#include <utility>

// Fakes for every port of the desktop's job peer session, and a host that wires them with a clock
// that only moves when a test moves it.
namespace peer_session_test {

using namespace ketplus::daemon::runner::peer;
namespace protocol = ketplus::runner::protocol;

struct SentFrame {
    QString connectionId;
    protocol::Channel channel;
    protocol::Frame frame;
};

class FakeLinks final : public PeerLinks {
  public:
    void send(const QString& connectionId, protocol::Channel channel,
              const QByteArray& message) override;
    void sendBulk(const QString& connectionId, const QString& transferId,
                  const QByteArray& bytes) override {
        bulk.append({connectionId, transferId, bytes});
    }
    [[nodiscard]] bool bulkWritable(const QString&) const override { return true; }
    void close(const QString& connectionId, const QString& reason) override {
        closes.append(connectionId + QLatin1Char(' ') + reason);
    }

    QList<SentFrame> sent; // decoded as the runner would
    QList<std::tuple<QString, QString, QByteArray>> bulk;
    QStringList closes;
    int undecodable = 0;
};

class FakeClock final : public PeerClock {
  public:
    [[nodiscard]] QDateTime now() const override { return time; }
    void arm(const QString&, PeerTimer timer, qint64 delayMs) override {
        deadlines[timer] = time.addMSecs(delayMs);
    }
    void disarm(const QString&, PeerTimer timer) override { deadlines.remove(timer); }
    [[nodiscard]] bool armed(PeerTimer timer) const { return deadlines.contains(timer); }

    QDateTime time = QDateTime(QDate(2026, 11, 18), QTime(9, 0), QTimeZone::UTC);
    QMap<PeerTimer, QDateTime> deadlines;
};

class FakeRecords final : public JobRecords {
  public:
    [[nodiscard]] std::optional<RecordView> view(const QString&) const override {
        return exists ? std::optional<RecordView>(record) : std::nullopt;
    }
    [[nodiscard]] RecordAnswer offer(const QString&, const QString& runnerId,
                                     qint64 attempt) override;
    [[nodiscard]] RecordAnswer dispatch(const QString&, const QString& runnerId) override;
    void accepted(const QString&, const QString& runnerId) override {
        calls.append(QStringLiteral("accepted ") + runnerId);
        record.accepted = record.dispatchedTo == runnerId;
    }
    void started(const QString&, const QString& runnerId) override {
        calls.append(QStringLiteral("started ") + runnerId);
        record.started = record.dispatchedTo == runnerId;
    }
    void rejected(const QString&, const QString& runnerId, const QString& code) override {
        calls.append(QStringLiteral("rejected %1 %2").arg(runnerId, code));
    }
    [[nodiscard]] RecordAnswer result(const QString&, const QString& runnerId,
                                      const protocol::JobResult& result) override;
    [[nodiscard]] RecordAnswer snapshotFailed(const QString&, const QString& runnerId,
                                              const QString& code) override;
    [[nodiscard]] QString jobOf(const QString& taskId) const override {
        return taskId == QStringLiteral("task-1") ? QStringLiteral("job-1") : QString();
    }
    void peerOpened(const QString&, const QString& runnerId) override {
        calls.append(QStringLiteral("peerOpened ") + runnerId);
    }
    void peerLost(const QString&, const QString& runnerId, const QDateTime& lastContact) override {
        calls.append(QStringLiteral("peerLost ") + runnerId);
        lostAt = lastContact;
    }

    bool exists = true;
    RecordView record;
    bool allowAnyOffer = false; // offers of other runners go through too
    QString offerCode;          // the answer to every offer, when set
    QString dispatchCode;       // the answer to every dispatch, when set
    QStringList resultCodes;    // answers to the next results, in order; then ok
    QStringList snapshotCodes;  // answers to the next snapshot failures, in order; then the rule
    QStringList calls;
    QList<protocol::JobResult> results;
    QDateTime lostAt;
};

class FakeSpecs final : public JobSpecSource {
  public:
    [[nodiscard]] std::optional<protocol::JobSpec> spec(const QString& jobId,
                                                        qint64 attempt) override;
    bool available = true;
    int asked = 0;
};

class FakeSnapshots final : public SnapshotPort {
  public:
    void start(const QString&, const QString& runnerId, const QMap<QString, QString>& held,
               Send sendFrame, SendBulk sendBulk) override {
        starts.append({runnerId, held});
        send = std::move(sendFrame);
        bulk = std::move(sendBulk);
    }
    void receive(const QString&, const protocol::Frame& frame) override { received.append(frame); }
    void bulkWritable(const QString&) override { ++writable; }
    void cancel(const QString&) override { ++cancels; }

    QList<std::pair<QString, QMap<QString, QString>>> starts;
    Send send;
    SendBulk bulk;
    QList<protocol::Frame> received;
    int writable = 0;
    int cancels = 0;
};

class MemorySpool final : public OutputSpool {
  public:
    [[nodiscard]] qint64 size() const override { return size_; }
    [[nodiscard]] bool append(const QByteArray& bytes) override;
    [[nodiscard]] bool markGap(qint64 toOffset) override;

    QByteArray bytes;
    qint64 size_ = 0;
    bool failing = false;
    int writes = 0; // successful appends and gaps
    QList<std::pair<qint64, qint64>> gaps;
};

inline const QString jobId = QStringLiteral("job-1");
inline const QString runnerId = QStringLiteral("runner-1");
inline const QString daemonVersion = QStringLiteral("1.2.3");

[[nodiscard]] protocol::JobSpec makeSpec(qint64 attempt = 1);
[[nodiscard]] protocol::Hello makeHello(const QString& connectionId,
                                        const QString& runner = runnerId, qint64 attempt = 1);
[[nodiscard]] QByteArray encodeFromRunner(const protocol::Frame& frame);

struct Host {
    Host();

    // Moves the clock, firing every timer that comes due on the way, earliest first.
    void advance(qint64 milliseconds);

    // An accepted offer with its channels open.
    void open(const QString& connectionId = QStringLiteral("c1"), const QString& runner = runnerId,
              qint64 attempt = 1);
    // Open and a matching `hello`.
    void connect(const QString& connectionId = QStringLiteral("c1"),
                 const QString& runner = runnerId, qint64 attempt = 1);
    // Through `job.accepted` on c1.
    void accept();
    // Through `job.started` on c1.
    void start();
    void deliver(const protocol::Frame& frame, const QString& connectionId = QStringLiteral("c1"));
    void deliverRaw(protocol::Channel channel, const QByteArray& message,
                    const QString& connectionId = QStringLiteral("c1"));

    [[nodiscard]] QStringList sentTypes() const;
    template <typename T> [[nodiscard]] QList<T> sentOf() const {
        QList<T> out;
        for (const SentFrame& sent : links.sent)
            if (const T* f = std::get_if<T>(&sent.frame))
                out.append(*f);
        return out;
    }

    FakeLinks links;
    FakeClock clock;
    FakeRecords records;
    FakeSpecs specs;
    FakeSnapshots snapshots;
    MemorySpool spool;
    std::unique_ptr<JobPeerSession> session;
};

} // namespace peer_session_test
