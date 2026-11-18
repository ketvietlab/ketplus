#pragma once

#include "pairing/PairingPins.h"
#include "peer/PeerChannels.h"
#include "peer/PeerConnector.h"
#include "peer/RelayLink.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

// Fakes for every port of the runner's Relay socket and peer connection, with a clock that only
// moves when a test moves it, and the signing vectors the offer is checked against.
namespace runner_peer_test {

using namespace ketplus::runner;
using namespace ketplus::runner::peer;

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
inline constexpr const char* offerBytes =
    R"(["ketplus",2,"runner-offer","pc_abcdefghijklmnopqrstuvwx","ws_alpha","m_desktop01",)"
    R"("run_aaaaaaaaaaaa",1,"job_42",1,"2026-11-18T03:00:00.000Z",)"
    R"("4c1b50f0e8a0a87e3f8b65de77023b97e3d974085dfb690332f1f839b34b0fbc"])";
inline constexpr const char* offerSignature =
    "4XAl_7mz9GmAsHmWHMsFJ8ZGI8DHAUIPWfn5Rk_3_utVEsqJM5R9G9UX0xOA6IWzGj3jrWi-MIFCzklp5cG3CA";
} // namespace vectors

// Values that must never reach a log line.
inline constexpr const char* tokenMarker = "TOKEN-SECRET";
inline constexpr const char* turnMarker = "TURN-SECRET";

QDateTime vectorTime();
QJsonArray iceServers();
pairing::RunnerPin activePin();
// An answer SDP naming one DTLS fingerprint (or none).
QString answerSdp(bool withFingerprint = true);
QString answerFingerprint();
// `signal.answer` signed by the desktop key of vector 0.
QJsonObject signedAnswer(const QString& connectionId, const QString& offerSdp, const QString& sdp,
                         const QDateTime& issuedAt, int keyVersion = vectors::machineKeyVersion,
                         const QString& jobId = QString::fromLatin1(vectors::jobId));
// Every log line written so far, by any test.
QStringList& logLines();
// Records log lines from here on; QTest installs its own handler for each test class.
void captureLogs();

class FakeClock final : public PeerClock {
  public:
    [[nodiscard]] QDateTime now() const override { return start.addMSecs(elapsed); }
    void arm(PeerTimer timer, qint64 delayMs) override { due[timer] = elapsed + delayMs; }
    void disarm(PeerTimer timer) override { due.remove(timer); }
    [[nodiscard]] bool armed(PeerTimer timer) const { return due.contains(timer); }
    [[nodiscard]] qint64 remaining(PeerTimer timer) const { return due.value(timer) - elapsed; }
    // Moves time on, firing each timer that falls due on the way.
    void advance(qint64 ms);

    std::function<void(PeerTimer)> fire;
    QDateTime start = vectorTime();
    qint64 elapsed = 0;
    QMap<PeerTimer, qint64> due;
};

class FakeRelaySocket final : public RelaySocket {
  public:
    void open(const QString& accessToken) override;
    bool send(const QJsonObject& message) override;
    void close() override {
        live = false;
        ++closes;
    }

    QStringList tokens; // one per open()
    QList<QJsonObject> sent;
    int closes = 0;
    bool live = false;
    int openedWhileLive = 0;
};

class FakeTokens final : public TokenSource {
  public:
    void current(Answer answer) override { take(QStringLiteral("current"), std::move(answer)); }
    void renew(Answer answer) override { take(QStringLiteral("renew"), std::move(answer)); }
    // Answers the oldest pending call.
    void answer(Outcome outcome);

    QStringList calls;
    QList<Answer> pending;
    bool autoAnswer = true;
    int issued = 0;

  private:
    void take(const QString& call, Answer answer);
};

class RecordingRelayListener final : public RelayLinkListener, public WakeupListener {
  public:
    void ready() override { events.append(QStringLiteral("ready")); }
    void lost() override { events.append(QStringLiteral("lost")); }
    void refused(int status) override { events.append(QStringLiteral("refused:%1").arg(status)); }
    void runnerRemoved() override { events.append(QStringLiteral("runnerRemoved")); }
    void tokenRejected() override { events.append(QStringLiteral("tokenRejected")); }
    void signal(const QJsonObject& message) override { signals_.append(message); }
    void jobAvailable(const QString& jobId) override;
    void jobCancel(const QString& jobId, const QString& leaseId) override;
    void runnerPaired(const QString& ticketId, const QString& pairState,
                      const QString& approvalSignature) override;
    void runnerUnpaired(const QString& machineId) override;

    QStringList events;
    QStringList wakeups;
    QList<QJsonObject> signals_;
};

struct TransportRecord {
    QJsonArray iceServers;
    bool offered = false;
    QStringList answers;      // SDPs given to acceptAnswer
    QStringList fingerprints; // fingerprints given to acceptAnswer
    QStringList candidates;   // "<mid>|<candidate>"
    QList<ketplus::daemon::transport::Packet> sent;
    quint64 bufferedEvents = 0;
    bool offerOk = true;
    bool acceptOk = true;
    bool closed = false;
    bool destroyed = false;
};

class FakeTransports final : public PeerTransportFactory {
  public:
    [[nodiscard]] std::unique_ptr<PeerTransport> create(PeerTransportEvents& events) override;
    [[nodiscard]] TransportRecord& last() { return *records.last(); }

    QList<std::shared_ptr<TransportRecord>> records;
    bool offerOk = true;
};

class FakeSigner final : public OfferSigner {
  public:
    [[nodiscard]] QString runnerId() const override;
    [[nodiscard]] int runnerKeyVersion() const override { return vectors::runnerKeyVersion; }
    [[nodiscard]] QByteArray sign(const QByteArray& bytes) const override;
};

class FakePins final : public PinLookup {
  public:
    [[nodiscard]] std::optional<pairing::RunnerPin>
    activePin(const QString& machineId) const override;
    std::optional<pairing::RunnerPin> pin = runner_peer_test::activePin();
};

class FakeSender final : public SignalSender {
  public:
    [[nodiscard]] bool relayReady() const override { return ready; }
    bool sendSignal(const QJsonObject& message) override;
    [[nodiscard]] QList<QJsonObject> ofType(const QString& type) const;

    bool ready = true;
    QList<QJsonObject> sent;
};

class RecordingChannelListener final : public PeerChannelListener {
  public:
    void channelOpened(const QString& connectionId) override;
    void channelLost(const QString& reason) override;
    void frameReceived(protocol::Channel channel, const QByteArray& message) override;
    void transferReceived(quint64 transferId, const QByteArray& bytes) override;
    void eventsWritable() override { events.append(QStringLiteral("writable")); }
    void attemptFailed(const QString& reason) override;
    void gaveUp(const QString& reason) override;

    QStringList events;
    QList<std::pair<protocol::Channel, QByteArray>> frames;
    QList<std::pair<quint64, QByteArray>> transfers;
};

// The connector and channels over fakes, wired as the runner will wire them.
struct PeerHarness {
    PeerHarness();
    // request() for job_42 and its signal.start.
    void request(int attempt = 1);
    // signal.started for `id`, expiring after `expiresInMs`.
    void started(const QString& id = QString::fromLatin1(vectors::connectionId),
                 qint64 expiresInMs = 60'000);
    // The transport's offer SDP is published and the signed offer sent.
    void describe(const QString& sdp = QString::fromLatin1(vectors::offerSdp));
    // A valid answer for the current offer.
    void answer(const QString& id = QString::fromLatin1(vectors::connectionId));
    // request → started → offer → answer → opened.
    void open();

    FakeClock clock;
    FakeSender sender;
    FakeTransports transports;
    FakeSigner signer;
    FakePins pins;
    RecordingChannelListener listener;
    PeerChannels channels{clock, listener};
    PeerConnector connector{sender, clock, transports, signer, pins, channels};
    QString offerSdp;
};

class RunnerRelayLinkTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { captureLogs(); }
    void readyNeedsTheRunnerRole();
    void readyMustComeInTime();
    void unauthorizedRenewsOnceThenBacksOff();
    void forbiddenAndUnknownRefusalsBackOff();
    void credentialExpiryRenewsAtOnceAfterAHealthySocket();
    void credentialExpiryOnAYoungSocketBacksOff();
    void removalStopsForGood();
    void tokenSourceOutcomes();
    void backoffRestartsOnlyAfterAHealthySocket();
    void stopAndStartKeepOneSocket();
    void lateTokenAnswersAreIgnored();
    void wakeupsAreCheckedThenHandedOver_data();
    void wakeupsAreCheckedThenHandedOver();
    void signalsGoToTheirListener();
};

class RunnerPeerConnectorTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { captureLogs(); }
    void offerIsSignedOverTheVectorBytes();
    void eachSignalErrorCode_data();
    void eachSignalErrorCode();
    void overlappingRequestsEndTheOldAttempt();
    void otherConnectionsAreIgnored();
    void desktopEndReasons_data();
    void desktopEndReasons();
    void desktopEndsOutsideAnAttemptAreIgnored();
    void answerFailures_data();
    void answerFailures();
    void aReplayedAnswerIsRefused();
    void anAnswerBeforeTheOfferEndsTheAttempt();
    void noActivePinStopsBeforeAnyStart();
    void aPinRemovedDuringAnAttemptStopsTheRetry();
    void theFingerprintComesFromTheSignedAnswer();
    void localCandidatesFollowTheOffer();
    void remoteCandidatesWaitForTheCheckedAnswer();
    void candidateLimits_data();
    void candidateLimits();
    void deadlineAndRetryDelays();
    void expiresAtShortensTheDeadline();
    void startsKeepTheirSpacingAcrossRequests();
    void relayLossDuringAnAttemptWaitsForReady();
    void relayLossLeavesAnOpenConnection();
    void runnerRemovalStopsAttempts();
    void anOpenConnectionThatDropsIsNotRetried();
    void aTransportThatClosesBeforeOpeningIsRetried();
};

class RunnerPeerChannelsTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { captureLogs(); }
    void openedAndLostAreReportedOnce();
    void sessionCloseReportsNothingLater();
    void framesPassThroughAsBytes();
    void receiveViolationsCloseTheConnection_data();
    void receiveViolationsCloseTheConnection();
    void sendKeepsToControlAndEvents();
    void eventsWritableIsReportedOncePerChange();
    void aFourMiBTransferArrivesWithItsId();
    void fourInterleavedTransfersAndAFifth();
    void transfersExpireAfterFiveMinutes();
    void anUnfinishedTransferDiesWithItsConnection();
    void requestConnectionAsksForTheCurrentJob();
};

class RunnerPeerLoopbackTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { captureLogs(); }
    void runnerAndDesktopExchangeFramesAndATransfer();
    void aTamperedAnswerNeverOpens();
    void relaySocketOpensAndReadsReady();
    void relaySocketReportsRefusals_data();
    void relaySocketReportsRefusals();
    void relaySocketReportsCloseCodes_data();
    void relaySocketReportsCloseCodes();
    void nothingSecretWasLogged();
};

} // namespace runner_peer_test
