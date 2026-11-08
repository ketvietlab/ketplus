#pragma once

#include <QJsonObject>

namespace ketplus::daemon {

// ProviderAdapter callbacks carry object results. Preserve null for existing void
// responses, but never turn a missing/scalar result or ambiguous envelope into success.
// Claude's sidecar permits message-only errors and string codes, so error member
// validation belongs to the provider-specific layer rather than this envelope guard.
[[nodiscard]] inline bool validProviderResponseEnvelope(const QJsonObject& object) {
    const bool hasResult = object.contains(QStringLiteral("result"));
    const bool hasError = object.contains(QStringLiteral("error"));
    if (hasResult == hasError)
        return false;
    if (hasError)
        return object.value(QStringLiteral("error")).isObject();
    const auto result = object.value(QStringLiteral("result"));
    return result.isObject() || result.isNull();
}

} // namespace ketplus::daemon
