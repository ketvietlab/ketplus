#include "peer/PeerConnector.h"

#include "peer/PeerLog.h"

#include <QRegularExpression>

#include <algorithm>

namespace ketplus::runner::peer {
namespace {

bool validConnectionId(const QString& id) {
    static const QRegularExpression pattern(QStringLiteral("^pc_[A-Za-z0-9_-]{24}$"));
    return pattern.match(id).hasMatch();
}

bool retriedSignalError(const QString& code) {
    return code != QLatin1StringView("not_paired") && code != QLatin1StringView("lease_required") &&
           code != QLatin1StringView("unsupported");
}

QString knownSignalError(const QString& code) {
    for (const char* known :
         {"host_offline", "not_paired", "lease_required", "rate_limited", "unsupported"})
        if (code == QLatin1StringView(known))
            return code;
    return QStringLiteral("signal_error");
}

// The reason of a desktop's signal.end, when it is at most 64 printable ASCII characters.
QString loggableCode(const QJsonValue& value) {
    const QString code = value.toString();
    if (code.isEmpty() || code.size() > 64)
        return QStringLiteral("unknown");
    for (const QChar c : code)
        if (c.unicode() < 0x21 || c.unicode() > 0x7e)
            return QStringLiteral("unknown");
    return code;
}

} // namespace

PeerConnector::PeerConnector(SignalSender& sender, PeerClock& clock,
                             PeerTransportFactory& transports, const OfferSigner& signer,
                             const PinLookup& pins, ConnectorEvents& events)
    : m_sender(sender), m_clock(clock), m_transports(transports), m_signer(signer), m_pins(pins),
      m_events(events) {}

PeerConnector::~PeerConnector() { dropTransport(); }

qint64 PeerConnector::nowMs() const { return m_clock.now().toMSecsSinceEpoch(); }

bool PeerConnector::attempting() const {
    return m_state == State::Starting || m_state == State::Offering ||
           m_state == State::Answering || m_state == State::Connecting;
}

PeerTransport* PeerConnector::openTransport() const {
    return m_state == State::Open ? m_transport.get() : nullptr;
}

void PeerConnector::dropTransport() {
    if (auto transport = std::move(m_transport))
        transport->close();
}

void PeerConnector::request(const PeerRequest& request) {
    const bool wasOpen = m_state == State::Open;
    close(QStringLiteral("replaced"));
    if (wasOpen) {
        m_events.connectionLost(QStringLiteral("replaced"));
        close(QStringLiteral("replaced")); // whatever the listener started meanwhile
    }
    m_request = request;
    m_retryDelayMs = attemptRetryBaseMs;
    scheduleStart(0);
}

void PeerConnector::close(const QString& reason) {
    if (attempting() && !m_context.connectionId.isEmpty())
        m_sender.sendSignal({{QStringLiteral("type"), QStringLiteral("signal.end")},
                             {QStringLiteral("connectionId"), m_context.connectionId},
                             {QStringLiteral("reason"), reason}});
    dropTransport();
    m_clock.disarm(PeerTimer::AttemptDeadline);
    m_clock.disarm(PeerTimer::AttemptRetry);
    m_state = State::Idle;
    m_request.reset();
    m_context = {};
}

void PeerConnector::fail(const QString& reason) {
    if (m_state != State::Open)
        return;
    qCInfo(lcRunnerPeer) << "peer connection closed:" << reason;
    close(reason);
    m_events.connectionLost(reason);
}

void PeerConnector::scheduleStart(const qint64 delayMs) {
    if (!m_request)
        return;
    if (!m_sender.relayReady()) {
        m_state = State::WaitRelay;
        return;
    }
    qint64 due = delayMs;
    if (m_lastStartMs)
        due = std::max(due, *m_lastStartMs + startSpacingMs - nowMs());
    if (due > 0) {
        m_state = State::WaitRetry;
        m_clock.arm(PeerTimer::AttemptRetry, due);
        return;
    }
    sendStart();
}

void PeerConnector::sendStart() {
    m_context = {};
    m_iceServers = {};
    m_offerSdp.clear();
    m_offerSent = false;
    m_remoteCandidates.clear();
    m_localCandidates.clear();
    m_remoteCount = 0;
    m_localCount = 0;
    m_remoteComplete = false;
    if (!m_sender.relayReady() ||
        !m_sender.sendSignal({{QStringLiteral("type"), QStringLiteral("signal.start")},
                              {QStringLiteral("jobId"), m_request->jobId},
                              {QStringLiteral("leaseId"), m_request->leaseId}})) {
        m_state = State::WaitRelay;
        return;
    }
    m_lastStartMs = nowMs();
    m_state = State::Starting;
    m_clock.arm(PeerTimer::AttemptDeadline, attemptDeadlineMs);
}

void PeerConnector::endAttempt(const QString& reason, const bool retry, const bool tellRelay) {
    if (tellRelay && !m_context.connectionId.isEmpty())
        m_sender.sendSignal({{QStringLiteral("type"), QStringLiteral("signal.end")},
                             {QStringLiteral("connectionId"), m_context.connectionId},
                             {QStringLiteral("reason"), reason}});
    dropTransport();
    m_clock.disarm(PeerTimer::AttemptDeadline);
    m_context = {};
    qCInfo(lcRunnerPeer) << "peer attempt ended:" << reason << (retry ? "(retrying)" : "");
    if (!retry) {
        m_clock.disarm(PeerTimer::AttemptRetry);
        m_state = State::Idle;
        m_request.reset();
        m_events.gaveUp(reason);
        return;
    }
    const qint64 delay = m_retryDelayMs;
    m_retryDelayMs = std::min(m_retryDelayMs * 2, attemptRetryCapMs);
    scheduleStart(delay);
    m_events.attemptFailed(reason);
}

void PeerConnector::timerFired(const PeerTimer timer) {
    if (timer == PeerTimer::AttemptRetry && m_state == State::WaitRetry)
        sendStart();
    else if (timer == PeerTimer::AttemptDeadline && attempting())
        endAttempt(QStringLiteral("attempt_timeout"), true);
}

void PeerConnector::relayReady() {
    if (m_state == State::WaitRelay)
        scheduleStart(0);
}

void PeerConnector::relayLost() {
    if (attempting())
        endAttempt(QStringLiteral("relay_lost"), true);
}

void PeerConnector::runnerRemoved() {
    if (!m_request || m_state == State::Open)
        return;
    close(QStringLiteral("runner_removed"));
    m_events.gaveUp(QStringLiteral("runner_removed"));
}

void PeerConnector::signal(const QJsonObject& message) {
    if (!attempting())
        return;
    const QString type = message.value(QStringLiteral("type")).toString();
    const QString id = message.value(QStringLiteral("connectionId")).toString();
    if (type == QLatin1StringView("signal.started")) {
        if (m_state == State::Starting)
            started(message);
        return;
    }
    if (type == QLatin1StringView("signal.error")) {
        if ((!id.isEmpty() && id == m_context.connectionId) ||
            (m_state == State::Starting && id.isEmpty()))
            signalError(message);
        return;
    }
    if (m_context.connectionId.isEmpty() || id != m_context.connectionId)
        return;
    if (type == QLatin1StringView("signal.answer"))
        answer(message);
    else if (type == QLatin1StringView("signal.candidate"))
        remoteCandidate(message);
    else if (type == QLatin1StringView("signal.end"))
        remoteEnd(message);
}

void PeerConnector::remoteEnd(const QJsonObject& message) {
    const QString code = loggableCode(message.value(QStringLiteral("reason")));
    qCInfo(lcRunnerPeer) << "peer attempt ended by the desktop:" << code;
    if (code == QLatin1StringView("already_dispatched") || code == QLatin1StringView("not_paired"))
        endAttempt(code, false, false);
    else if (code == QLatin1StringView("job_mismatch"))
        endAttempt(code, true, false);
    else
        endAttempt(QStringLiteral("remote_ended"), true, false);
}

void PeerConnector::signalError(const QJsonObject& message) {
    const QString code = knownSignalError(message.value(QStringLiteral("code")).toString());
    endAttempt(code, retriedSignalError(code));
}

void PeerConnector::started(const QJsonObject& message) {
    const QString id = message.value(QStringLiteral("connectionId")).toString();
    const QJsonValue iceServers = message.value(QStringLiteral("iceServers"));
    if (!validConnectionId(id) || !iceServers.isArray()) {
        endAttempt(QStringLiteral("invalid_started"), true);
        return;
    }
    m_context.connectionId = id;
    const QDateTime expiresAt = QDateTime::fromString(
        message.value(QStringLiteral("expiresAt")).toString(), Qt::ISODateWithMs);
    if (expiresAt.isValid()) {
        const qint64 deadline =
            std::min(*m_lastStartMs + attemptDeadlineMs, expiresAt.toMSecsSinceEpoch());
        m_clock.arm(PeerTimer::AttemptDeadline, std::max<qint64>(0, deadline - nowMs()));
    }
    const auto pin = m_pins.activePin(m_request->machineId);
    if (!pin) {
        endAttempt(answerErrorCode(AnswerError::NotPinned), false);
        return;
    }
    m_context.workspaceId = pin->workspaceId;
    m_context.machineId = m_request->machineId;
    m_context.runnerId = m_signer.runnerId();
    m_context.runnerKeyVersion = m_signer.runnerKeyVersion();
    m_context.jobId = m_request->jobId;
    m_context.attempt = m_request->attempt;
    m_iceServers = iceServers.toArray();
    m_state = State::Offering;
    m_transport = m_transports.create(*this);
    if (!m_transport || !m_transport->offer(m_iceServers))
        endAttempt(QStringLiteral("offer_failed"), true);
}

void PeerConnector::localDescription(const QString& sdp) {
    if (m_state != State::Offering)
        return;
    const auto offer = signedOffer(m_context, sdp, m_clock.now(), m_signer);
    if (!offer) {
        endAttempt(QStringLiteral("offer_failed"), true);
        return;
    }
    m_offerSdp = sdp;
    if (!m_sender.sendSignal(*offer)) {
        endAttempt(QStringLiteral("relay_lost"), true);
        return;
    }
    m_offerSent = true;
    m_state = State::Answering;
    const auto pending = std::exchange(m_localCandidates, {});
    for (const auto& candidate : pending)
        if (!sendLocalCandidate(candidate.candidate, candidate.mid))
            return;
}

void PeerConnector::localCandidate(const QString& candidate, const QString& mid) {
    if (m_state != State::Offering && m_state != State::Answering && m_state != State::Connecting)
        return;
    if (!m_offerSent)
        m_localCandidates.append({candidate, mid});
    else
        sendLocalCandidate(candidate, mid);
}

bool PeerConnector::sendLocalCandidate(const QString& candidate, const QString& mid) {
    if (!candidate.isEmpty() &&
        (++m_localCount > candidateLimit || candidate.toUtf8().size() > candidateSizeLimit ||
         mid.size() > candidateMidLimit)) {
        endAttempt(QStringLiteral("candidate_limit"), true);
        return false;
    }
    m_sender.sendSignal(
        {{QStringLiteral("type"), QStringLiteral("signal.candidate")},
         {QStringLiteral("connectionId"), m_context.connectionId},
         {QStringLiteral("candidate"),
          candidate.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(candidate)},
         {QStringLiteral("sdpMid"), mid}});
    return true;
}

void PeerConnector::answer(const QJsonObject& message) {
    if (m_state == State::Starting || m_state == State::Offering) {
        endAttempt(QStringLiteral("answer_before_offer"), true);
        return;
    }
    if (m_state != State::Answering)
        return;
    const auto check =
        checkAnswer(message, m_context, m_offerSdp, m_pins.activePin(m_context.machineId),
                    m_clock.now(), m_usedAnswers);
    if (!check.answer) {
        endAttempt(answerErrorCode(check.error), true);
        return;
    }
    m_state = State::Connecting;
    if (!m_transport->acceptAnswer(check.answer->sdp, check.answer->fingerprint)) {
        endAttempt(QStringLiteral("answer_failed"), true);
        return;
    }
    const auto pending = std::exchange(m_remoteCandidates, {});
    for (const auto& candidate : pending) {
        if (m_state != State::Connecting)
            return;
        if (!m_transport->candidate(candidate.candidate, candidate.mid)) {
            endAttempt(QStringLiteral("invalid_candidate"), true);
            return;
        }
    }
}

void PeerConnector::remoteCandidate(const QJsonObject& message) {
    const QJsonValue candidate = message.value(QStringLiteral("candidate"));
    const QJsonValue mid = message.value(QStringLiteral("sdpMid"));
    const bool end = candidate.isNull();
    if (m_remoteComplete || (!end && !candidate.isString()) ||
        (!mid.isString() && !mid.isNull() && !mid.isUndefined()) ||
        mid.toString().size() > candidateMidLimit ||
        (!end && (candidate.toString().isEmpty() || ++m_remoteCount > candidateLimit ||
                  candidate.toString().toUtf8().size() > candidateSizeLimit))) {
        endAttempt(QStringLiteral("candidate_limit"), true);
        return;
    }
    m_remoteComplete = end;
    const RemoteCandidate entry{end ? QString() : candidate.toString(), mid.toString()};
    if (m_state != State::Connecting) {
        m_remoteCandidates.append(entry);
        return;
    }
    if (!m_transport->candidate(entry.candidate, entry.mid))
        endAttempt(QStringLiteral("invalid_candidate"), true);
}

void PeerConnector::opened() {
    if (m_state != State::Connecting)
        return;
    m_state = State::Open;
    m_clock.disarm(PeerTimer::AttemptDeadline);
    m_retryDelayMs = attemptRetryBaseMs;
    m_remoteCandidates.clear();
    m_localCandidates.clear();
    qCInfo(lcRunnerPeer) << "peer connection open";
    m_events.connectionOpened(m_context.connectionId);
}

void PeerConnector::received(const Channel channel, const QByteArray& bytes, const bool binary) {
    if (m_state == State::Open)
        m_events.received(channel, bytes, binary);
}

void PeerConnector::writable() {
    if (m_state == State::Open)
        m_events.writable();
}

void PeerConnector::closed() {
    if (m_state == State::Open)
        fail(QStringLiteral("peer_closed"));
    else if (attempting())
        endAttempt(QStringLiteral("peer_closed"), true);
}

} // namespace ketplus::runner::peer
