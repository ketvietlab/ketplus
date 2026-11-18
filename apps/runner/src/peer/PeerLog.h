#pragma once

#include <QLoggingCategory>

// Lines about the Relay socket and the peer connection. They carry codes, states and counts
// only: never SDP, candidates, ICE server credentials, tokens or signatures.
Q_DECLARE_LOGGING_CATEGORY(lcRunnerPeer)
