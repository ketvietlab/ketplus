#pragma once

#include <QLatin1StringView>

// Capability names exchanged in `hello` and `welcome`.
namespace ketplus::runner::protocol::capability {

// The runner peer protocol. `hello` must carry it; a peer without it closes the connection.
inline constexpr QLatin1StringView runnerProtocol{"runner.protocol/1"};

} // namespace ketplus::runner::protocol::capability
