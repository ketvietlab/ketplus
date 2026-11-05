# Settings architecture

KetPlus CM owns the reusable settings shell and preferences that are useful
without a proprietary service. Product-specific consumers may add pages through
`SettingsPage`, but CM does not know the meaning or storage contract of those
pages.

## Current public settings

`AppearanceSettings` is a user-wide, normalized value containing:

- interface font size;
- editor font family, font size, and line height;
- terminal font size and line height;
- preview font size and line height.

The existing `editor/*` keys remain unchanged. New settings use
`appearance/interfaceFontSizePixels`, `terminal/*`, and `preview/*`. Font sizes
are bounded before use, and each content line height is at least its font size.
Interface line height is deliberately automatic because control geometry comes
from the design-system density rather than document typography.

`SettingsDialog` retains its editor-only constructor and signal for compatible
consumers. New code should pass `AppearanceSettings` and `ThemeManager`, listen
for `appearanceSettingsSaved`, and apply the complete snapshot atomically.

## Extending the dialog

A consumer can subclass `SettingsPage`, buffer its values locally, and add the
page with `SettingsDialog::addPage`. `resetToDefaults()` and `apply()` participate
in the dialog-wide Reset and Save actions. Pages override `hasChanges()` and emit
`changed()` whenever their buffered state changes so the shared Save action tracks
dirty state accurately. Cancel must leave persisted state unchanged.

Extensions must keep proprietary values in their own namespaces and modules.
Do not add account, AI-provider, remote-control, entitlement, or other private
contracts to CM merely to display their settings page.

## Theme selection

The Appearance page currently lists the built-in Két Light and Két Dark themes
and supports following the operating-system appearance. Selection previews use
`ThemeManager::previewMode`; preview never changes the committed `QSettings`
value. Save commits the mode and Cancel restores the committed theme.

The UI reserves import entry points for VS Code, JetBrains, and Open VSX themes,
but those actions remain disabled until a secure importer, immutable catalog,
resolver, compatibility report, and transactional storage are implemented.
Import must never execute extension code.
