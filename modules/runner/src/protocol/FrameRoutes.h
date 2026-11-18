#pragma once

#include <QAnyStringView>
#include <QLatin1StringView>
#include <QString>

#include <span>

// Which channel and direction each frame type uses, and its size limit.
namespace ketplus::runner::protocol {

// The three reliable ordered data channels of the peer connection. `Bulk` carries raw snapshot
// chunk transfers only, so no JSON frame is routed to it.
enum class Channel { Control, Events, Bulk };

enum class Direction { RunnerToDesktop, DesktopToRunner };

struct Route {
    QLatin1StringView type;
    Channel channel;
    bool fromRunner;  // the runner may send it
    bool fromDesktop; // the desktop may send it
    qsizetype maxBytes;
};

[[nodiscard]] std::span<const Route> allRoutes();
// Null for a type this protocol version does not know.
[[nodiscard]] const Route* routeFor(QAnyStringView type);
[[nodiscard]] bool allows(const Route& route, Direction direction);

} // namespace ketplus::runner::protocol
