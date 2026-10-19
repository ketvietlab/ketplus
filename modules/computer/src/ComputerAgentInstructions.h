#pragma once

#include <QString>

namespace ketplus::computer_tools {

// Routing policy appended to the host instructions of Claude and Cursor turns
// (ADR 0026, amending ADR 0014). Model guidance only; the agent app enforces access.
[[nodiscard]] QString agentInstructions(bool computerAvailable);

} // namespace ketplus::computer_tools
