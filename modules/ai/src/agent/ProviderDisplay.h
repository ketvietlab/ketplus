#pragma once

#include <QString>

namespace ketplus::provider_display {
// Provider ids remain protocol values; every visible name uses the product's spelling.
inline QString name(const QString& provider) {
    if (provider == QStringLiteral("copilot"))
        return QStringLiteral("GitHub Copilot");
    return provider.left(1).toUpper() + provider.mid(1);
}
} // namespace ketplus::provider_display
