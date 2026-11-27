# 0021: Default agent skills shipped with KetPlus

Status: accepted
Date: 2026-10-08

## Context

Mockups, Atlas bundles and UI work in KetPlus should follow the same standards
without the user mentioning a skill every turn: KetAtlas for mocks and the Atlas
screen, the enterprise-grade bar for UI and frontend work, and Két Design System
when the user has not chosen a design system. User skill folders differ between
machines and providers (the Claude sidecar loads no setting sources, so Claude
sessions never see `~/.claude/skills`), and phone-started turns bypass the
desktop's skill wrapping.

## Decision

- KetPlus ships `assets/agent-skills/{ket-atlas,enterprise-grade,ket-design-system}`
  in `KetPlus.app/Contents/Resources/agent-skills`. Desktop (`Contents/MacOS`) and
  the daemon (`Contents/Helpers`) both resolve `../Resources/agent-skills`.
  `VerifyBundle.cmake` rejects a bundle missing one of them.
- `hostAgentInstructions` appends a `[KetPlus default skills]` block naming each
  shipped skill, its `SKILL.md` path and when to read it. Every provider and
  transport that already receives the host policy (Codex developer instructions,
  Claude system prompt append, Cursor context block) therefore gets it, whether
  the turn starts on the desktop or the phone.
- The block tells the agent to read a skill only when the task matches, so
  unrelated turns pay for a few lines, not the skill text. When the user has not
  named a design system, Két Design System counts as their choice, which answers
  the `$ket-atlas` design-system question without asking.
- A project or user skill with the same name takes precedence. The skill picker
  lists the shipped skills as `Built-in`, below project and user skills, so they
  can also be mentioned (`$ket-atlas`) or enabled.

## Consequences

- Updating a shipped skill is a KetPlus release: copy the new text into
  `assets/agent-skills` and advance the product version. The ket-design-system copy
  keeps its MIT `LICENSE`; the other two are Két Việt proprietary.
- The policy is model guidance, not enforcement.

## Alternatives considered

- Auto-enable the skills in the picker: every turn would carry about 40 KB of
  skill text, and phone turns would still miss them.
- Install into the user's `~/.claude/skills` or `~/.codex/skills`: changes user
  configuration outside the app and still misses Claude sidecar sessions.
