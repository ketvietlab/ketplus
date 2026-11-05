# Public-Safe CM Brief

This brief is safe to share with the public KetPlus CM team. It intentionally excludes private screens, private flows, transcripts, account details, daemon internals, and proprietary Atlas/AI implementation.

## Goal

Expose public native primitives so private KetPlus Desktop can map a locked design to Qt Widgets, Scintilla, Qt Quick consumers, settings, search, Git/workspace surfaces, and terminal/preview chrome without editing `vendor/ketplus-cm` in place.

## Tokens And Metrics

Public CM may implement or expose:

- Semantic color groups for canvas, panel, sidebar, list, tabs, input, focus, selection, success, warning, danger, info, code, diff, terminal, preview, and disabled states.
- Geometry metrics for compact native UI: navigation rail width, control heights, padding scale, gaps, one-pixel borders, 5 to 7 px interactive radii, and two-pixel active indicators.
- Typography mapping for Inter UI font, fractional pixel metrics, line-height, and platform monospace source text.
- Icon plumbing for Lucide-style monochrome stroke icons with stable viewBox, stroke width, tint, alignment, and disabled/hover/active states.
- Settings host APIs for reusable appearance/typography content and deep linking to public or private pages.
- Generic safe workspace/Git primitives for validation and non-private filesystem operations.

## Generic Component States

Public CM can receive tests/fixtures for generic states only:

- Idle, hover, focus, selected, active, disabled, loading, error, empty, and reduced-motion states.
- Tab active/inactive, side navigation selected/unselected, list row selected/running/success/error as generic status variants.
- Dialog sizing, focus trap, Close/Escape semantics, button groups, segmented controls, text inputs, and scroll containers.
- Editor, diff, Markdown preview, terminal, search, settings, and workspace tree chrome.

## Explicit Exclusions

Do not include in public CM:

- Private AI Workspace transcript logic, provider JSON, command output policy, daemon IPC, account/session IDs, mobile relay semantics, Router, billing, entitlement, or remote-control implementation.
- Private Atlas builder/library workflow semantics, proprietary bundle index logic, or viewer internals.
- Any user transcript, QR secret, auth credential, device key, machine-specific path, or private production screen copy.

## Handoff Needed From CM

- Public release tag, not a moving develop pin.
- Tests and visual primitive evidence.
- Changelog describing exported APIs and compatibility.
- No changes to MPL-covered files hidden inside private Desktop.

Desktop Integration will advance the private gitlink only after CM release, public tests, private build/test verification, and submodule review.
