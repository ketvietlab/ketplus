#include "run/Backoff.h"

#include <algorithm>

namespace ketplus::runner::app {

qint64 Backoff::next() {
    const int doublings = std::min(m_failures, 6); // 64 seconds is already past the cap
    ++m_failures;
    const qint64 base = std::min(backoffCapMs, backoffBaseMs << doublings);
    const double jitter = std::clamp(m_random(), 0.0, 1.0) * 0.5;
    return std::min(backoffCapMs, base + static_cast<qint64>(static_cast<double>(base) * jitter));
}

} // namespace ketplus::runner::app
