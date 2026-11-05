#pragma once

#include <QString>

namespace ketplus::agent_message {

// Agents colour nodes with `style` and `classDef` fills but rarely name a text colour, so a
// pale fill kept the dark theme's pale labels. A fill without `color:` gains the dark or
// light text that reads on it.
[[nodiscard]] QString withReadableMermaidLabels(const QString& diagram);

} // namespace ketplus::agent_message
