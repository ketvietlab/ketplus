#pragma once

#include <QString>

namespace ketplus::daemon {

// One agent-to-agent message (ADR 0027). Sender and receiver are conversations on this host.
struct AgentMessageRecord final {
    QString id;
    QString chainId;
    int hop{1};
    QString fromSession;
    qint64 fromGeneration{0};
    QString fromNumber;
    QString fromTitle;
    QString fromProvider;
    QString toSession;
    qint64 toGeneration{0};
    QString toNumber;
    // "request", "reply" or "notice".
    QString kind;
    QString replyTo;
    bool expectReply{false};
    bool urgent{false};
    QString body;
    // "queued", "delivered", "answered", "held", "expired" or "failed".
    QString state;
    // How it reached the receiver: "auto" (a turn it started), "user_turn" or "steer".
    QString delivery;
    qint64 deliveredTurn{0};
    QString error;
    QString createdAt;
    QString deliveredAt;
    QString expiresAt;
};

} // namespace ketplus::daemon
