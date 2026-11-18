#pragma once
#include "TeamLanguage.h"
#include <QLocale>
namespace ketplus::sidebar_filter {
inline constexpr auto name =
    QT_TRANSLATE_NOOP("SidebarFilter", "Filter repositories and conversations");
inline constexpr auto placeholder =
    QT_TRANSLATE_NOOP("SidebarFilter", "Filter repos / conversations");
inline constexpr auto help =
    QT_TRANSLATE_NOOP("SidebarFilter", "Filter by repository name/path or conversation title (%1)");
inline constexpr auto empty =
    QT_TRANSLATE_NOOP("SidebarFilter", "No matching repositories or conversations");
// Reuse the compiled Qt catalog, using the UI locale rather than a selected team's language.
inline QString text(const char* source) {
    return remote::team_language::text(remote::team_language::fromLocale(QLocale().name()),
                                       "SidebarFilter", source);
}
} // namespace ketplus::sidebar_filter
