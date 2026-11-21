#pragma once

#include <QtGlobal>

#include <functional>

// Delay before retrying a failed Relay call: 1 second, doubled after each failure, plus 0-50%
// jitter, never above 60 seconds. A success resets it.
namespace ketplus::runner::app {

inline constexpr qint64 backoffBaseMs = 1'000;
inline constexpr qint64 backoffCapMs = 60'000;

class Backoff {
  public:
    explicit Backoff(std::function<double()> random) : m_random(std::move(random)) {}

    [[nodiscard]] qint64 next();
    void reset() { m_failures = 0; }

  private:
    std::function<double()> m_random;
    int m_failures = 0;
};

} // namespace ketplus::runner::app
