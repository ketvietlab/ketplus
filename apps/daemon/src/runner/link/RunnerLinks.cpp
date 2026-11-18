#include "runner/link/RunnerLinks.h"

#include "runner/link/LinkLog.h"
#include "runner/link/RunnerLinkState.h"

namespace ketplus::daemon::runner::link {

RunnerLinks::RunnerLinks(SignalOut& out, LinkClock& clock, LinkTransportFactory& transports,
                         const MachineIdentity& identity, const RunnerPins& pins,
                         LinkEvents& events)
    : m_out(out), m_clock(clock), m_transports(transports), m_identity(identity), m_pins(pins),
      m_events(events) {}

RunnerLinks::~RunnerLinks() {
    for (const QString& id : m_links.keys())
        drop(id);
}

bool RunnerLinks::owns(const QString& connectionId) const { return m_links.contains(connectionId); }

RunnerLinks::Link* RunnerLinks::find(const QString& connectionId) const {
    const auto it = m_links.constFind(connectionId);
    return it == m_links.cend() ? nullptr : it.value().get();
}

int RunnerLinks::count(const bool open) const {
    int n = 0;
    for (const auto& link : m_links)
        if ((link->state == Link::State::Open) == open)
            ++n;
    return n;
}

void RunnerLinks::sendEnd(const QString& connectionId, const QString& reason) {
    m_out.sendSignal({{QStringLiteral("type"), QStringLiteral("signal.end")},
                      {QStringLiteral("connectionId"), connectionId},
                      {QStringLiteral("reason"), reason}});
}

void RunnerLinks::signal(const QJsonObject& message) {
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QLatin1StringView("signal.offer")) {
        if (isRunnerOffer(message))
            offer(message);
        return;
    }
    const QString id = message.value(QStringLiteral("connectionId")).toString();
    Link* link = find(id);
    if (!link)
        return;
    if (type == QLatin1StringView("signal.candidate"))
        remoteCandidate(*link, message);
    else if (type == QLatin1StringView("signal.end") || type == QLatin1StringView("signal.error"))
        remoteEnd(id,
                  message.value(type == QLatin1StringView("signal.end") ? QStringLiteral("reason")
                                                                        : QStringLiteral("code")));
}

void RunnerLinks::offer(const QJsonObject& message) {
    const OfferCheck check = checkOffer(message, m_identity, m_pins, m_clock.now(), m_used);
    if (!check.offer) {
        const QString reason = offerErrorReason(check.error);
        if (reason.isEmpty()) {
            qCInfo(lcRunnerLink) << "runner offer dropped";
            return;
        }
        qCInfo(lcRunnerLink) << "runner offer refused:" << reason;
        sendEnd(check.connectionId, reason);
        return;
    }
    const VerifiedOffer& offer = *check.offer;

    // The newest offer of a runner replaces its pending attempt.
    for (const auto& link : m_links.values())
        if (link->state == Link::State::Answering && link->offer.runnerId == offer.runnerId)
            endAttempt(link->offer.connectionId, QStringLiteral("superseded"));

    bool runnerOpen = false;
    for (const auto& link : m_links)
        if (link->state == Link::State::Open && link->offer.runnerId == offer.runnerId)
            runnerOpen = true;
    if (count(false) >= pendingAttemptLimit ||
        (!runnerOpen && count(true) >= openConnectionLimit)) {
        qCInfo(lcRunnerLink) << "runner offer refused: peer_limit";
        sendEnd(offer.connectionId, QStringLiteral("peer_limit"));
        return;
    }

    const RecordAnswer answer =
        m_events.offered(offer.connectionId, offer.runnerId, offer.jobId, offer.attempt);
    if (!answer.ok()) {
        qCInfo(lcRunnerLink) << "runner offer refused by the job side:"
                             << loggableCode(QJsonValue(answer.code));
        sendEnd(offer.connectionId, answer.code);
        return;
    }
    if (m_links.contains(offer.connectionId))
        return;

    auto link = std::make_shared<Link>(*this, offer);
    m_links.insert(offer.connectionId, link);
    m_clock.arm(offer.connectionId, attemptDeadlineMs);
    link->transport = m_transports.create(*link);
    if (!link->transport || !link->transport->answer(offer.sdp, offer.iceServers)) {
        endAttempt(offer.connectionId, QStringLiteral("answer_failed"));
        return;
    }
    if (link->dropped)
        return;
    link->applied = true;
    const auto pending = std::exchange(link->pendingCandidates, {});
    for (const auto& candidate : pending) {
        if (link->dropped)
            return;
        if (!link->transport->candidate(candidate.candidate, candidate.mid)) {
            endAttempt(offer.connectionId, QStringLiteral("invalid_candidate"));
            return;
        }
    }
    qCInfo(lcRunnerLink) << "runner offer answered";
}

void RunnerLinks::remoteCandidate(Link& link, const QJsonObject& message) {
    const QString id = link.offer.connectionId;
    const auto stop = [this, &link, &id](const QString& reason) {
        if (link.state == Link::State::Open)
            fail(id, reason);
        else
            endAttempt(id, reason);
    };
    const QJsonValue candidate = message.value(QStringLiteral("candidate"));
    const QJsonValue mid = message.value(QStringLiteral("sdpMid"));
    if (link.remoteDone || !(candidate.isString() || candidate.isNull()) ||
        !(mid.isString() || mid.isNull() || mid.isUndefined()) ||
        mid.toString().size() > candidateMidLimit) {
        stop(QStringLiteral("invalid_candidate"));
        return;
    }
    const QString text = candidate.toString();
    if (text.isEmpty()) {
        link.remoteDone = true;
    } else {
        if (text.toUtf8().size() > candidateSizeLimit || ++link.remoteCandidates > candidateLimit) {
            stop(QStringLiteral("candidate_limit"));
            return;
        }
    }
    if (!link.applied) {
        link.pendingCandidates.append({text, mid.toString()});
        return;
    }
    const auto keep = m_links.value(id);
    if (!link.transport->candidate(text, mid.toString()) && !link.dropped)
        stop(QStringLiteral("invalid_candidate"));
}

void RunnerLinks::remoteEnd(const QString& connectionId, const QJsonValue& code) {
    Link* link = find(connectionId);
    // An end after the channels opened is ignored: the connection carries on.
    if (!link || link->state != Link::State::Answering)
        return;
    qCInfo(lcRunnerLink) << "runner peer attempt ended by the other side:" << loggableCode(code);
    endAttempt(connectionId, QStringLiteral("remote_ended"), false);
}

void RunnerLinks::timerFired(const QString& connectionId) {
    Link* link = find(connectionId);
    if (link && link->state == Link::State::Answering)
        endAttempt(connectionId, QStringLiteral("attempt_timeout"));
}

void RunnerLinks::localDescription(Link& link, const QString& sdp) {
    if (link.answerSent || link.state != Link::State::Answering)
        return;
    const auto answer = signedAnswer(link.offer, sdp, m_clock.now(), m_identity);
    if (!answer) {
        endAttempt(link.offer.connectionId, QStringLiteral("answer_failed"));
        return;
    }
    link.answerSent = true;
    m_out.sendSignal(*answer);
}

void RunnerLinks::localCandidate(Link& link, const QString& candidate, const QString& mid) {
    if (link.localDone)
        return;
    if (candidate.isEmpty()) {
        link.localDone = true;
    } else if (++link.localCandidates > candidateLimit) {
        link.localDone = true;
        if (link.state == Link::State::Answering)
            endAttempt(link.offer.connectionId, QStringLiteral("candidate_limit"));
        return;
    }
    m_out.sendSignal({{QStringLiteral("type"), QStringLiteral("signal.candidate")},
                      {QStringLiteral("connectionId"), link.offer.connectionId},
                      {QStringLiteral("candidate"),
                       candidate.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(candidate)},
                      {QStringLiteral("sdpMid"), mid}});
}

void RunnerLinks::opened(Link& link) {
    if (link.state != Link::State::Answering)
        return;
    const QString id = link.offer.connectionId;
    link.state = Link::State::Open;
    m_clock.disarm(id);
    // A newer connection of a runner replaces its open one, which is not reported lost.
    for (const auto& other : m_links.values())
        if (other.get() != &link && other->state == Link::State::Open &&
            other->offer.runnerId == link.offer.runnerId) {
            qCInfo(lcRunnerLink) << "runner peer connection closed: superseded";
            drop(other->offer.connectionId);
        }
    qCInfo(lcRunnerLink) << "runner peer connection open";
    m_events.channelsOpened(id);
}

void RunnerLinks::transportClosed(Link& link) {
    const QString id = link.offer.connectionId;
    if (link.state == Link::State::Open)
        fail(id, QStringLiteral("peer_closed"));
    else
        endAttempt(id, QStringLiteral("peer_closed"));
}

void RunnerLinks::endAttempt(const QString& connectionId, const QString& reason,
                             const bool tellRelay) {
    const auto link = drop(connectionId);
    if (!link)
        return;
    qCInfo(lcRunnerLink) << "runner peer attempt ended:" << reason;
    if (tellRelay)
        sendEnd(connectionId, reason);
    m_events.offerEnded(connectionId);
}

void RunnerLinks::fail(const QString& connectionId, const QString& reason) {
    const auto link = drop(connectionId);
    if (!link)
        return;
    qCInfo(lcRunnerLink) << "runner peer connection lost:" << reason;
    m_events.connectionLost(connectionId);
}

std::shared_ptr<RunnerLinks::Link> RunnerLinks::drop(const QString& connectionId) {
    auto link = m_links.take(connectionId);
    if (!link)
        return link;
    link->dropped = true;
    m_clock.disarm(connectionId);
    if (link->transport)
        link->transport->close();
    return link;
}

void RunnerLinks::close(const QString& connectionId, const QString& reason) {
    const auto link = drop(connectionId);
    if (!link)
        return;
    // The reason stays local; the Relay only learns that the attempt is over.
    qCInfo(lcRunnerLink) << "runner peer connection closed:" << loggableCode(QJsonValue(reason));
    if (link->state == Link::State::Answering)
        sendEnd(connectionId, QStringLiteral("closed"));
}

} // namespace ketplus::daemon::runner::link
