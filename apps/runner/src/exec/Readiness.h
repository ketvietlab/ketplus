#pragma once

#include <QString>

// Whether the runner can take a job's command now.
namespace ketplus::runner::exec {

struct Readiness {
    enum class State { Unknown, Ready, Unavailable };
    State state = State::Unknown;
    QString code; // with Unavailable: containment_unavailable or cleanup_unverified

    [[nodiscard]] bool ready() const { return state == State::Ready; }
    bool operator==(const Readiness&) const = default;
};

} // namespace ketplus::runner::exec
