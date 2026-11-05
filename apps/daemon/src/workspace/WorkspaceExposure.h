#pragma once

#include <QString>

namespace ketplus::daemon::workspace_exposure {

// Paths whose content is usually a credential (.env files, private keys, auth configs).
// Paired devices never see them in a tree, search result, file, diff or image.
[[nodiscard]] bool isSecretPath(const QString& relativePath);

} // namespace ketplus::daemon::workspace_exposure
