# English application UI, prompt-driven team content

Status: accepted product direction; implementation T30.
Supersedes the UI and language-priority rules of [ADR 0125](0125-team-language.md).

The application owns English chrome: menus, labels, buttons, placeholders, helper text,
validation, status descriptions and newly generated fixed daemon templates. Team content
is a separate concern. Titles, goals, criteria, notes, messages, decisions and findings
written by users or agents follow the current user prompt and explicit content-language
instructions. The UI locale and the operating system never select that language.

`team.language` is a content preference: `auto` (new-team default), `en` or `vi`.
`auto` follows the prompt without attempting a character-based language classifier. Explicit
language instructions in the prompt win; otherwise the prompt's language wins. A saved
`en`/`vi` preference is a fallback only when those and the established content context do
not establish a language. Mixed or ambiguous prompts retain that established context. This also
allows content in languages beyond the two legacy preference values. The desktop labels
the setting **Content language**, with **Follow prompt**, **English**, **Vietnamese**.
It never changes the preference automatically while the user types or based on OS locale.

Old stored `en`/`vi` preferences remain readable and round-trip unchanged. Missing language
in stored records or new create requests uses `auto`; invalid wire values still fail
validation. This changes preference metadata only, never historical text. This is additive for current local and
paired clients; an older client that rejects `auto` needs its own compatibility update.
No private-client implementation is included here.

No historical content is migrated, translated or normalized. Previously generated
Vietnamese updates and stored decision choices remain in Vietnamese and are answered by
matching their original stored text. New fixed system templates are English. Agent-authored
content remains untouched; instructions steer future content, not existing records.

The shared language facade never installs a QTranslator. Scope/ambient APIs remain no-ops
for source compatibility and `current()` reports English. Fixed `text()` formatting uses
English source strings. Historical Qt catalogs are retained and checked for completeness;
new English chrome does not require a Vietnamese catalog entry. Date labels in desktop
settings/machine status use an explicit English locale. No global locale is imposed on
user documents, numeric inputs or external/native operating-system dialogs.

Offline fixtures prove generated instructions and data preservation; they do not prove a
particular provider will comply. No live provider call is part of acceptance.
