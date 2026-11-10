# ADR 0103: Settings changes apply as they are made

Status: accepted (Jay, 2026-11-10); replaces the staged Save / Cancel footer.

The Settings dialog staged every edit in a draft. The theme was a preview that
Cancel rolled back, and Save changes applied the appearance, the theme and each
extension page together. The design mock has no Save bar, and Jay chose to apply
every change at once rather than keep the mock's in-page Save changes.

`SettingsDialog::applyChanges()` now runs on every control change. It emits
`appearanceSettingsSaved` and `settingsSaved` when the appearance snapshot differs
from the last one it applied. It sets the theme mode when the selection changes.
It applies each extension page whose `hasChanges()` is true. Opening the dialog
applies nothing. The footer keeps only **Restore defaults**, which resets the
appearance, the theme (Follow system) and every extension page, and applies the
result. There is no Save, Cancel or Ctrl+Return; closing the dialog or pressing
Escape keeps what was applied, so `reject()` no longer cancels a theme preview.

The desktop's merge with a newer snapshot from another window is unchanged: fields
the open dialog did not edit keep the newer values. `RefactorWave1ContractTest` and
`SettingsDialogTest` record the new contract.

## Terminal and Markdown sizes follow the editor and the interface

Jay decided on 2026-11-10 to remove the Terminal and Markdown font size rows, as the
mock does. `AppearanceSettings::normalized()` now derives both sizes: terminal text
uses the editor's size and line height, and Markdown is one pixel over the interface
size, with the former 15/26 line rhythm. Stored `terminal/*` and `preview/*` values
are still written but no longer read, so an older release that reads them still opens.
The Typography preview drops its Markdown lines, and the Pro page follows the mock's
order: Interface font, Interface font size, Conversation width, Editor font, Editor font size.
ADR 0104 separates that size from the interface zoom.
