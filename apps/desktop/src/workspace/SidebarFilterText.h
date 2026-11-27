#pragma once
#include "TeamLanguage.h"
namespace ketplus::sidebar_filter {
inline constexpr auto name =
    QT_TRANSLATE_NOOP("SidebarFilter", "Filter repositories and conversations");
inline constexpr auto placeholder =
    QT_TRANSLATE_NOOP("SidebarFilter", "Filter repos / conversations");
inline constexpr auto help =
    QT_TRANSLATE_NOOP("SidebarFilter", "Filter by repository name/path or conversation title (%1)");
inline constexpr auto empty =
    QT_TRANSLATE_NOOP("SidebarFilter", "No matching repositories or conversations");
// Fixed English chrome, independent of the OS and selected team content.
inline QString text(const char* source) {
    return remote::team_language::text(remote::team_language::english, "SidebarFilter", source);
}
} // namespace ketplus::sidebar_filter
