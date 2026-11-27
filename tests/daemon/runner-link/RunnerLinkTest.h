#pragma once

#include "runner/link/RunnerLinks.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

// Fakes for every port of the desktop's runner peer links, with a clock that only moves when a
// test moves it, and the signing vectors of the pairing tests.
namespace runner_link_test {

using namespace ketplus::daemon::runner::link;
namespace pairing = ketplus::runner::pairing;

// Vector 0 of the pairing tests: keys, ids, the offer SDP and its signature.
namespace vectors {
inline constexpr const char* runnerSeed =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
inline constexpr const char* runnerPublicKey =
    "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8";
inline constexpr const char* desktopSeed =
    "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f";
inline constexpr const char* desktopPublicKey =
    "29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd7";
inline constexpr const char* workspaceId = "ws_alpha";
inline constexpr const char* machineId = "m_desktop01";
inline constexpr int machineKeyVersion = 1;
inline constexpr const char* runnerId = "run_aaaaaaaaaaaa";
inline constexpr int runnerKeyVersion = 1;
inline constexpr const char* issuedAt = "2026-11-18T03:00:00.000Z";
inline constexpr const char* connectionId = "pc_abcdefghijklmnopqrstuvwx";
inline constexpr const char* jobId = "job_42";
inline constexpr const char* offerSdp = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\n";
inline constexpr const char* offerSignature =
    "4XAl_7mz9GmAsHmWHMsFJ8ZGI8DHAUIPWfn5Rk_3_utVEsqJM5R9G9UX0xOA6IWzGj3jrWi-MIFCzklp5cG3CA";
// The desktop's answer to the vector offer, issued at the same time, computed apart from
// this code with an RFC 8032 reference implementation.
inline constexpr const char* answerSdp = "v=0\r\no=- 3 4 IN IP4 127.0.0.1\r\n";
inline constexpr const char* answerBytes =
    R"(["ketplus",2,"runner-answer","pc_abcdefghijklmnopqrstuvwx","ws_alpha","m_desktop01",1,)"
    R"("run_aaaaaaaaaaaa","job_42",)"
    R"("4c1b50f0e8a0a87e3f8b65de77023b97e3d974085dfb690332f1f839b34b0fbc",)"
    R"("2026-11-18T03:00:00.000Z",)"
    R"("ff0f5415b17f667014df0df3b418bacf239d2ae04c68696fea7b5cd651a5d696"])";
inline constexpr const char* answerSignature =
    "sLgP35gx9obUbcN0GR_JZzs-o0d7-qw80LH9mKs-SRR65A3S_x-ucQjWGKtZG4nDZ051KinWiGG8W7n8ahZdAA";
} // namespace vectors

// Values that must never reach a log line.
inline constexpr const char* turnMarker = "TURN-SECRET";
inline constexpr const char* sdpMarker = "SDP-SECRET";
inline constexpr const char* candidateMarker = "CANDIDATE-SECRET";

QDateTime vectorTime();
QJsonArray iceServers();
// `pc_` and 24 characters ending in `n`.
QString connectionId(int n);
// A runner id of the vector's form ending in `n`.
QString runnerId(int n);
pairing::DesktopRunnerPin
runnerPin(const QString& runnerId = QString::fromLatin1(vectors::runnerId));

// The fields of one runner offer, signed by the vector runner key unless the test changes them
// after signing.
struct OfferFields {
    QString connectionId = QString::fromLatin1(vectors::connectionId);
    QString runnerId = QString::fromLatin1(vectors::runnerId);
    int runnerKeyVersion = vectors::runnerKeyVersion;
    QString jobId = QString::fromLatin1(vectors::jobId);
    int attempt = 1;
    QString issuedAt = QString::fromLatin1(vectors::issuedAt);
    QString sdp = QString::fromLatin1(vectors::offerSdp);
};
// `signal.offer` as the Relay forwards it: signed fields plus `runnerId` and `iceServers`.
QJsonObject offer(const OfferFields& fields = {});

// Every log line written so far, by any test.
QStringList& logLines();
// Records log lines from here on; QTest installs its own handler for each test class.
void captureLogs();

class FakeClock final : public LinkClock {
  public:
    [[nodiscard]] QDateTime now() const override { return start.addMSecs(elapsed); }
    void arm(const QString& connectionId, qint64 delayMs) override {
        due[connectionId] = elapsed + delayMs;
    }
    void disarm(const QString& connectionId) override { due.remove(connectionId); }
    // Moves time on, firing each deadline that falls due on the way.
    void advance(qint64 ms);

    QDateTime start = vectorTime();
    qint64 elapsed = 0;
    QMap<QString, qint64> due;
    std::function<void(const QString&)> fire;
};

// What one fake transport saw; outlives the transport.
struct TransportRecord {
    LinkTransportEvents* events = nullptr;
    bool alive = true;
    bool closed = false;
    QString offerSdp;
    QJsonArray iceServers;
    QList<std::pair<QString, QString>> candidates;
    QList<ketplus::daemon::transport::Packet> sent;
    QMap<int, quint64> buffered;
    bool answerOk = true;
    bool candidateOk = true;
    bool sendOk = true;
    std::function<void()> duringAnswer; // runs inside answer(), before it returns
};

class FakeTransport final : public LinkTransport {
  public:
    explicit FakeTransport(std::shared_ptr<TransportRecord> record) : m_record(std::move(record)) {}
    ~FakeTransport() override { m_record->alive = false; }
    bool answer(const QString& offerSdp, const QJsonArray& iceServers) override;
    bool candidate(const QString& candidate, const QString& mid) override;
    bool send(TransportChannel channel, const QByteArray& bytes, bool binary) override;
    [[nodiscard]] quint64 buffered(TransportChannel channel) const override {
        return m_record->buffered.value(int(channel));
    }
    void close() override { m_record->closed = true; }

  private:
    std::shared_ptr<TransportRecord> m_record;
};

class FakeTransports final : public LinkTransportFactory {
  public:
    [[nodiscard]] std::unique_ptr<LinkTransport> create(LinkTransportEvents& events) override;
    // The record of the transport made for the n-th accepted offer.
    [[nodiscard]] TransportRecord& at(qsizetype n) const { return *records.at(n); }
    [[nodiscard]] TransportRecord& last() const { return *records.last(); }

    QList<std::shared_ptr<TransportRecord>> records;
    bool answerOk = true;
    std::function<void()> duringAnswer;
};

class FakeIdentity final : public MachineIdentity {
  public:
    [[nodiscard]] QString workspaceId() const override {
        return QString::fromLatin1(vectors::workspaceId);
    }
    [[nodiscard]] QString machineId() const override {
        return QString::fromLatin1(vectors::machineId);
    }
    [[nodiscard]] int machineKeyVersion() const override { return vectors::machineKeyVersion; }
    [[nodiscard]] QByteArray sign(const QByteArray& bytes) const override;

    bool fail = false;
};

class FakePins final : public RunnerPins {
  public:
    [[nodiscard]] std::optional<pairing::DesktopRunnerPin>
    pin(const QString& runnerId) const override;

    // Pins by runner id; every runner of the tests is pinned unless removed here.
    QMap<QString, pairing::DesktopRunnerPin> overrides;
    QSet<QString> unpinned;
};

class FakeOut final : public SignalOut {
  public:
    void sendSignal(const QJsonObject& message) override { sent.append(message); }
    [[nodiscard]] QList<QJsonObject> ofType(const QString& type) const;
    // The reasons of every signal.end sent, in order.
    [[nodiscard]] QStringList ends() const;

    QList<QJsonObject> sent;
};

class RecordingEvents final : public LinkEvents {
  public:
    [[nodiscard]] RecordAnswer offered(const QString& connectionId, const QString& runnerId,
                                       const QString& jobId, qint64 attempt) override;
    void offerEnded(const QString& connectionId) override {
        events.append(QStringLiteral("ended ") + connectionId);
    }
    void channelsOpened(const QString& connectionId) override {
        events.append(QStringLiteral("opened ") + connectionId);
    }
    void frameReceived(const QString& connectionId, FrameChannel channel,
                       const QByteArray& message) override;
    void bulkWritable(const QString& connectionId) override {
        events.append(QStringLiteral("writable ") + connectionId);
    }
    // Recorded with the reason of the log line written just before.
    void connectionLost(const QString& connectionId) override;

    QString code; // the answer to offered()
    QStringList events;
    QList<std::tuple<QString, FrameChannel, QByteArray>> frames;
};

// The links over fakes.
struct LinkHarness {
    LinkHarness();
    // One `signal.*` message from the Relay.
    void deliver(const QJsonObject& message) { links.signal(message); }
    // An accepted offer for `fields`, its answer described and its channels opened.
    TransportRecord& open(const OfferFields& fields = {});

    FakeOut out;
    FakeClock clock;
    FakeTransports transports;
    FakeIdentity identity;
    FakePins pins;
    RecordingEvents events;
    RunnerLinks links{out, clock, transports, identity, pins, events};
};

class RunnerLinkOfferTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { captureLogs(); }
    void takesTheVectorOffer();
    void refusesOffers_data();
    void refusesOffers();
    void refusesAnOfferTheJobSideRefuses();
    void dropsUnreadableConnectionIds_data();
    void dropsUnreadableConnectionIds();
    void dropsAReplayWithoutTouchingTheAttempt();
    void anUnverifiedOfferTakesNoPlace();
    void aFullUsedTableAnswersPeerLimit();
    void ignoresOffersOfDevices_data();
    void ignoresOffersOfDevices();
    void signsTheAnswerVector();
    void refusesAnAnswerItCannotSign();
    void endsWhenTheTransportCannotAnswer();
};

class RunnerLinkLifecycleTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { captureLogs(); }
    void passesCandidatesBothWays();
    void holdsCandidatesUntilTheOfferIsApplied();
    void refusesCandidates_data();
    void refusesCandidates();
    void endsAtLocalCandidateLimit();
    void aBadCandidateAfterOpenIsLost();
    void endsAtTheDeadline();
    void newestOfferReplacesThePendingAttempt();
    void aRefusedNewestOfferStillEndsTheOldAttempt();
    void anOpenConnectionStaysUntilTheNewOneOpens();
    void aFailedNewAttemptLeavesTheOpenConnection();
    void limitsAttemptsAndConnections();
    void endsOnTheOtherSidesEnd_data();
    void endsOnTheOtherSidesEnd();
    void ignoresAnEndAfterOpen();
    void reportsATransportThatCloses();
    void closeReportsNothing();
    void destructionReportsNothing();
};

class RunnerLinkChannelsTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { captureLogs(); }
    void sendsFrames();
    void keepsTheConnectionForFramesItCannotSend();
    void reportsAFailedSend();
    void receivesFrames();
    void closesOnBrokenChannelRules_data();
    void closesOnBrokenChannelRules();
    void sendsTransfers();
    void keepsTheConnectionForTransfersItCannotSend_data();
    void keepsTheConnectionForTransfersItCannotSend();
    void closesOnBulkOverflow();
    void tellsWhenBulkIsWritableAgain();
    void logsNoSecrets();
};

class RunnerLinkLoopbackTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { captureLogs(); }
    void runnerAndDesktopExchangeFramesAndATransfer();
    void aTamperedOfferNeverOpens();
};

} // namespace runner_link_test
