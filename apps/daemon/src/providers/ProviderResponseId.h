#pragma once

#include <QJsonValue>

#include <optional>

namespace ketplus::daemon {

// Adapters assign positive integer JSON ids to their calls. A string, boolean or
// fractional number must not consume an entry in that numeric pending-call map.
[[nodiscard]] inline std::optional<quint64> providerResponseId(const QJsonValue& value) {
    if (!value.isDouble())
        return std::nullopt;
    const qint64 id = value.toInteger(-1);
    if (id <= 0 || value.toDouble() != static_cast<double>(id))
        return std::nullopt;
    return static_cast<quint64>(id);
}

} // namespace ketplus::daemon
