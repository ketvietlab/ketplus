#pragma once

namespace ketplus::toast_avoidance {
// Shared by the local and Machine composers so a hidden view cannot clear another owner.
inline constexpr char ownerProperty[] = "_ketplus_toast_composer_owner";
} // namespace ketplus::toast_avoidance
