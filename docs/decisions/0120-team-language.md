# ADR 0120: A team works in its leader's language

Status: accepted (2026-11-13). Amends ADR 0114 (team record, Updates, decisions).

## Context

Everything a team shows was English: the team page ("Waiting for you", "Recommended", task
states), the Updates the daemon writes ("D-21 answered", "Automatic turns: N, M used"), the
decisions the daemon raises itself ("The team used its 40 automatic turns. Continue?") and the
digest it sends the leader. A user who works with the leader in Vietnamese read a Vietnamese
conversation next to an English team page. The repository had no translation set-up of any
kind.

## Decision

1. **A team stores a language.** `language` is `"en"` or `"vi"` (ISO 639-1; more can be
   added). It is part of the team record, the team JSON the desktop reads and the relay
   `teams.remote/1` payload. A team saved before this field, or created by a client that sends
   none, is English. Any other value is refused with `invalid_payload`.
2. **Set at creation, changed in settings.** New Team has a Language field. It starts from the
   system's language and follows the goal while the user has not picked one: a goal written
   with Vietnamese letters selects Vietnamese. The leader is a new conversation, so there is no
   leader conversation whose language could be read; the goal is the user's first words to the
   leader. The team menu changes it later with `team.update {language}`, which logs an Update
   of kind `language` (`data: {from, to}`), written in the new language.
3. **Qt Linguist, one catalog.** English is the source text. The Vietnamese text lives in
   `modules/remote/translations/ketplus_teams_vi.ts`, compiled by `lrelease` at build time
   into the `ketplus_team_language` library's resources. Every team text, desktop or daemon,
   is in one context, `Team`: the desktop's helper `tr()` functions translate under contexts
   lupdate cannot see, and a team text means the same wherever it shows.
   `update_team_translations.py` (target `ketplus_team_language_lupdate`) refreshes the
   catalog from the sources and keeps the translations already written.
4. **The desktop keeps its `tr()` calls.** One translator is installed in the application. It
   answers only for team contexts (`Team*`, `ketplus::Team*`, `ketplus::team_page::*`,
   `NewTeamForm`, `AddMemberForm`) and only in a team's language: inside a
   `team_language::Scope`, which the team page opens while it builds, or, on the GUI thread,
   in the language of the team page that is shown, so menus and dialogs opened from it and the
   texts they change later follow it. The rest of the application is unaffected.
5. **The daemon writes in the team's language.** Updates, task history entries, the
   decisions the daemon raises (budget, stall, merge, first plan, empty role), the round-limit
   question and the digest sent to the leader use `say(team.language, QT_TRANSLATE_NOOP("Team",
   ...))`. Task states and roles inside those texts are translated too; an English team keeps
   the stored words (`in_progress`) exactly as before.
6. **Choices are known by place.** A fixed choice of a daemon decision is matched by its
   position among the decision's choices, not by its English text, so a decision raised in
   one language is answered after the team changed to the other.
7. **The leader writes in the team's language.** For a Vietnamese team the team instructions
   tell the leader to write task titles, criteria, notes, lanes, decisions and what it tells
   the user in Vietnamese, and the developers and the reviewer their task notes and
   questions. Code, commit messages, public pull request bodies and the reviewer's exact
   verdict line stay English.

## Consequences

- A text the daemon wrote stays in the language it was written in: changing the language
  does not rewrite earlier Updates or history.
- The words a member writes (task titles, notes, decision questions) are as the member wrote
  them; only the instructions steer their language.
- The phone reads `language` from the relay payload; its own strings are its own work.
- A new team text needs a translation: the catalog refresh lists the ones without one, and
  `ketplus_team_page_language_tests` fails when the team page shows an English fixed text for
  a Vietnamese team.

## Alternatives considered

- **A per-app language.** One team speaks Vietnamese while another speaks English on the same
  computer, so the language belongs to the team, not to the app.
- **Hand-written string tables.** Hundreds of desktop `tr()` calls and daemon texts would need
  a second mechanism; Qt Linguist is already part of Qt and gives plurals and tooling.
- **Translating on the desktop only.** The daemon's Updates and decisions are what the user
  reads most, and the phone shows them too; they must be written in the team's language.
