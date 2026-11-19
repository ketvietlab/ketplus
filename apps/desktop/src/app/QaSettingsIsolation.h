#pragma once

#include <QString>

namespace ketplus {

// Run before any default QSettings or AppUpdater instance is created.
// Returns an error message if isolation was requested and startup must stop.
[[nodiscard]] QString configureQaSettingsIsolation(const QString& organization,
                                                   const QString& application);

} // namespace ketplus
