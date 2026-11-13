# ADR 0121: Canonical KetPlus color scheme format

Status: accepted
Date: 2026-11-13

## Context

KetPlus needs editable, installable color schemes and can accept imperfect
imports from VS Code and JetBrains. Their formats and rendering models differ;
runtime dependence on either would couple persistence and consumers to an
external application. Existing consumers use a shared ThemePalette.

## Decision

Use versioned plain `.ketscheme.json` as the only canonical color-scheme format.
Implement sparse QColor-based models, strict validation, lossless unknown-data
preservation and deterministic resolution under `core/src/ui/theme/`. Both
editions share this contract. External importers translate into it and return
structured mapping/approximation/omission reports.

Version 1 supports only embedded light/dark inheritance bases, independent
high-contrast kind, UI/editor/syntax/terminal/diff tokens, explicit syntax style
resets, and provenance/license metadata. Hex alpha is RGBA. Keep the current
ThemePalette as a compatibility projection while component consumers migrate.
Represent both built-ins in resource JSON and test parity with current palettes.

Store user schemes under AppDataLocation/color-schemes as immutable ID-named
files. Use QLockFile and QSaveFile for concurrent-safe, atomic additions and
locked removals. Generate UUIDs independently of names and source paths.
Unknown or corrupt saved IDs fall back to an embedded scheme without preventing
startup. Reject unsupported schema versions rather than silently downgrading.

## Consequences

Importers are lossy translators with explicit reports, not external theme or
extension runtimes. Unknown JSON survives serialization but has no rendering
semantics. The original source scheme remains sparse while resolution produces
complete values. There are no inheritance cycles, remote includes or package
execution. Package installation and selection persistence remain separate work.
The schema and detailed behavior are documented in `../color-schemes.md`.

## Alternatives considered

Persisting VS Code or JetBrains data directly would force all consumers to
understand that external format. A fully expanded snapshot would freeze defaults
and discard inheritance intent. An indexed package catalog is unnecessary for
immutable single-file schemes; package assets can be managed independently.
