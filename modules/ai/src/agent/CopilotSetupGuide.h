#pragma once

#include <QJsonObject>
#include <QVariantMap>

namespace ketplus::provider_setup {
// Platform is explicit for deterministic setup-guide coverage on all supported hosts.
[[nodiscard]] QString copilotInstallCommand(const QString& platform);
[[nodiscard]] QVariantMap copilotGuide(const QJsonObject& provider, const QJsonObject& turnError,
                                       const QString& platform);
} // namespace ketplus::provider_setup
