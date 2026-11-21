#include "ui/theme/ColorSchemeKitAdapter.h"

namespace ketplus {
QHash<QString, QColor> colorSchemeKitOverrides(const ColorScheme& authored) {
    const auto scheme = resolveColorScheme(authored);
    const auto base = resolveColorScheme(builtinColorScheme(
        (scheme.kind == SchemeKind::Dark ||
         (scheme.kind == SchemeKind::HighContrast && scheme.extends == u"builtin-dark"))
            ? SchemeKind::Dark
            : SchemeKind::Light));
    QHash<QString, QColor> result;
    struct Mapping {
        const char* path;
        const char* token;
    };
    // Aliases are explicit. Syntax roles never map onto accent/chart tokens:
    // changing code colors must not recolor unrelated application controls.
    static constexpr Mapping ui[] = {{"chrome.appBackground", "app-bg"},
                                     {"chrome.pageBackground", "page-bg"},
                                     {"chrome.pageBackground", "color-canvas"},
                                     {"chrome.panelBackground", "panel-bg"},
                                     {"chrome.panelBackground", "color-surface"},
                                     {"chrome.panelSubtle", "panel-bg-subtle"},
                                     {"chrome.panelSubtle", "color-surface-subtle"},
                                     {"chrome.surfaceRaised", "surface-raised"},
                                     {"chrome.surfaceHover", "surface-hover"},
                                     {"chrome.surfaceHover", "color-surface-hover"},
                                     {"chrome.textMain", "text-main"},
                                     {"chrome.textMain", "color-ink"},
                                     {"chrome.textSecondary", "text-secondary"},
                                     {"chrome.textSecondary", "color-ink-secondary"},
                                     {"chrome.textMuted", "text-muted"},
                                     {"chrome.textMuted", "color-ink-muted"},
                                     {"chrome.textDisabled", "text-disabled"},
                                     {"chrome.border", "panel-border"},
                                     {"chrome.border", "border-default"},
                                     {"chrome.border", "color-line"},
                                     {"chrome.borderStrong", "border-strong"},
                                     {"chrome.borderStrong", "color-line-strong"},
                                     {"chrome.accent", "accent"},
                                     {"chrome.accent", "color-accent"},
                                     {"chrome.accentHover", "accent-hover"},
                                     {"chrome.accentHover", "color-accent-hover"},
                                     {"chrome.accentActive", "accent-active"},
                                     {"chrome.accentSubtle", "accent-subtle"},
                                     {"chrome.accentSubtle", "color-accent-soft"},
                                     {"chrome.accentMuted", "accent-muted"},
                                     {"chrome.accentText", "accent-text"},
                                     {"chrome.positive", "positive"},
                                     {"chrome.positive", "color-positive"},
                                     {"chrome.warning", "warning"},
                                     {"chrome.warning", "color-warning"},
                                     {"chrome.danger", "danger"},
                                     {"chrome.danger", "color-danger"},
                                     {"chrome.info", "info"},
                                     {"chrome.info", "color-info"},
                                     {"sidebar.bg", "sidebar-bg"},
                                     {"sidebar.border", "sidebar-border"},
                                     {"input.bg", "input-bg"},
                                     {"input.bg", "interactive-bg"},
                                     {"input.fg", "input-text"},
                                     {"input.border", "input-border"},
                                     {"list.hover", "interactive-hover"},
                                     {"list.selection", "interactive-active"},
                                     {"tab.activeFg", "tab-active-text"},
                                     {"focus.border", "color-focus"},
                                     {"focus.border", "input-border-focus"},
                                     {"button.bg", "action-primary-bg"},
                                     {"button.hover", "action-primary-hover"},
                                     {"button.active", "action-primary-active"},
                                     {"button.fg", "action-primary-ink"},
                                     {"button.fg", "color-on-accent"}};
    for (const auto& mapping : ui) {
        const auto path = QString::fromLatin1(mapping.path);
        if (scheme.ui.value(path) != base.ui.value(path))
            result.insert(QString::fromLatin1(mapping.token), scheme.ui.value(path));
    }
    for (int i = 0; i < 16; ++i) {
        const auto key = QStringLiteral("ansi%1").arg(i);
        if (scheme.terminal.value(key) != base.terminal.value(key))
            result.insert(QStringLiteral("terminal-ansi-%1").arg(i), scheme.terminal.value(key));
    }
    return result;
}
} // namespace ketplus
