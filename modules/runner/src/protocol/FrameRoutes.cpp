#include "protocol/FrameRoutes.h"

#include "protocol/FrameLimits.h"

#include <array>

namespace ketplus::runner::protocol {
namespace {

using limits::maxFrameBytes;
using limits::maxFrameBytes16K;
using limits::maxFrameBytes1K;
using limits::maxFrameBytes4K;

constexpr bool runner = true;
constexpr bool desktop = true;
constexpr bool none = false;

constexpr std::array<Route, 23> kRoutes{{
    {QLatin1StringView("hello"), Channel::Control, runner, none, maxFrameBytes},
    {QLatin1StringView("welcome"), Channel::Control, none, desktop, maxFrameBytes4K},
    {QLatin1StringView("job.spec"), Channel::Control, none, desktop, maxFrameBytes},
    {QLatin1StringView("job.accepted"), Channel::Control, runner, none, maxFrameBytes1K},
    {QLatin1StringView("job.rejected"), Channel::Control, runner, none, maxFrameBytes1K},
    {QLatin1StringView("job.resume"), Channel::Control, none, desktop, maxFrameBytes1K},
    {QLatin1StringView("snapshot.offer"), Channel::Control, none, desktop, maxFrameBytes4K},
    {QLatin1StringView("snapshot.want"), Channel::Control, runner, none, maxFrameBytes1K},
    {QLatin1StringView("snapshot.chunk"), Channel::Control, none, desktop, maxFrameBytes},
    {QLatin1StringView("snapshot.ack"), Channel::Control, runner, none, maxFrameBytes1K},
    {QLatin1StringView("snapshot.ready"), Channel::Control, runner, none, maxFrameBytes1K},
    {QLatin1StringView("snapshot.error"), Channel::Control, runner, none, maxFrameBytes1K},
    {QLatin1StringView("job.started"), Channel::Control, runner, none, maxFrameBytes1K},
    {QLatin1StringView("job.output"), Channel::Events, runner, none, maxFrameBytes},
    {QLatin1StringView("job.output.gap"), Channel::Events, runner, none, maxFrameBytes1K},
    {QLatin1StringView("job.output.ack"), Channel::Control, none, desktop, maxFrameBytes1K},
    {QLatin1StringView("job.output.resume"), Channel::Control, none, desktop, maxFrameBytes1K},
    {QLatin1StringView("job.cancel"), Channel::Control, none, desktop, maxFrameBytes1K},
    {QLatin1StringView("job.result"), Channel::Control, runner, none, maxFrameBytes16K},
    {QLatin1StringView("job.result.ack"), Channel::Control, none, desktop, maxFrameBytes1K},
    {QLatin1StringView("ping"), Channel::Control, runner, desktop, maxFrameBytes1K},
    {QLatin1StringView("pong"), Channel::Control, runner, desktop, maxFrameBytes1K},
    {QLatin1StringView("close"), Channel::Control, runner, desktop, maxFrameBytes1K},
}};

} // namespace

std::span<const Route> allRoutes() { return kRoutes; }

const Route* routeFor(const QAnyStringView type) {
    for (const Route& route : kRoutes) {
        if (QAnyStringView::equal(type, route.type))
            return &route;
    }
    return nullptr;
}

bool allows(const Route& route, const Direction direction) {
    return direction == Direction::RunnerToDesktop ? route.fromRunner : route.fromDesktop;
}

} // namespace ketplus::runner::protocol
