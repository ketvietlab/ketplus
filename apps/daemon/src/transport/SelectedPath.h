#pragma once

#include <QDateTime>
#include <QString>
#include <rtc/candidate.hpp>

namespace ketplus::daemon::transport {
inline QString candidateKind(rtc::Candidate::Type type) {
    switch (type) {
    case rtc::Candidate::Type::Host:
        return QStringLiteral("host");
    case rtc::Candidate::Type::ServerReflexive:
        return QStringLiteral("srflx");
    case rtc::Candidate::Type::PeerReflexive:
        return QStringLiteral("prflx");
    case rtc::Candidate::Type::Relayed:
        return QStringLiteral("relay");
    default:
        return QStringLiteral("unknown");
    }
}

// Accept native enums, never candidate strings, URLs, addresses or credentials.
inline QString selectedPathLog(rtc::Candidate::Type local, rtc::Candidate::Type remote,
                               qint64 epoch, bool reopened, const QDateTime& at) {
    // This backend uses libjuice, whose TURN connection is UDP-only. This is the
    // local TURN transport; the remote client's relay transport is not in its SDP.
    const auto turn = local == rtc::Candidate::Type::Relayed   ? "udp"
                      : local == rtc::Candidate::Type::Unknown ? "unknown"
                                                               : "none";
    return QStringLiteral("ice-path local=%1 remote=%2 turn=%3 epoch=%4 event=%5 at=%6")
        .arg(candidateKind(local), candidateKind(remote), QString::fromLatin1(turn))
        .arg(epoch)
        .arg(reopened ? QStringLiteral("ice-connected") : QStringLiteral("open"),
             at.toUTC().toString(Qt::ISODateWithMs));
}
} // namespace ketplus::daemon::transport
