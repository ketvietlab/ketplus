#pragma once

#include "protocol/Frames.h"

#include <QJsonObject>

namespace ketplus::runner::protocol::detail {

// The JSON object of a frame without any check. Senders use encode(); this exists so tests can
// build messages that break a limit and feed them to decode().
[[nodiscard]] QJsonObject toObject(const Frame& frame);

} // namespace ketplus::runner::protocol::detail
