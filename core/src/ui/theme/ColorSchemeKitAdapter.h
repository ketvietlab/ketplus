#pragma once
#include "ui/theme/ColorScheme.h"
#include <QHash>

namespace ketplus {
// Deltas from the published branch's immutable built-in base, using only existing kit
// tokens. Keeping the base empty preserves every kit default, including alpha.
[[nodiscard]] QHash<QString, QColor> colorSchemeKitOverrides(const ColorScheme& scheme);
} // namespace ketplus
