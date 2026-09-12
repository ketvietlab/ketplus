# Token and metric mapping

Source: public Két Design System `0.1.7`,
`6f923d1e13958ce740e50bccab45efc56ccd2f84`,
`packages/design-system/src/foundations/tokens.css`.

This file separates canonical KDS tokens from observed native implementation.
Observed implementation values are not a blessed visual baseline unless marked
approved by Integration/QA.

## Color roles

| KDS semantic/component role | Current native field/use | Qt palette/QSS destination | Status |
| --- | --- | --- | --- |
| `--kv-page-bg` | `ThemePalette::pageBackground` | `QPalette::Window`, `QMainWindow`, `QDialog`, tab background | Implemented |
| `--kv-app-bg` | `ThemePalette::appBackground` | Reserved/native app background | Implemented field, limited use |
| `--kv-sidebar-bg` | `sidebarBackground` | Activity bar, explorer, settings navigation | Implemented |
| `--kv-panel-bg` | `panelBackground` | `QPalette::Base`, panels, tables, dialogs, cards | Implemented |
| `--kv-panel-bg-subtle` | `panelSubtle` | Terminal/preview headers, status areas, footer/header bands | Implemented |
| `--kv-surface-raised` | `surfaceRaised` | Menus, tooltips, labels | Implemented |
| `--kv-surface-hover` | `surfaceHover` | Hover states, splitter hover alternatives | Implemented |
| `--kv-text-main` | `textMain` | `QPalette::WindowText`, `Text`, default widget text | Implemented |
| `--kv-text-secondary` | `textSecondary` | Button text, labels, headings | Implemented |
| `--kv-text-muted` | `textMuted` | Status, hints, secondary paths | Implemented |
| `--kv-text-disabled` | `textDisabled` | Disabled activity/primary states | Implemented |
| `--kv-panel-border` | `border` | Dividers, panel borders, table borders | Implemented |
| `--kv-border-strong` | `borderStrong` | Menus, inputs, tooltips, scroll handles | Implemented |
| `--kv-interactive-bg` | `interactiveBackground` | `QPalette::Button`, controls | Implemented |
| `--kv-interactive-hover` | `interactiveHover` | Control hover states | Implemented |
| `--kv-interactive-active` | `interactiveActive` | Pressed controls | Implemented |
| `--kv-interactive-disabled` | Proposed `interactiveDisabled` | Disabled controls | Proposed |
| `--kv-accent` | `accent` | Active tabs, checked navigation, primary actions | Implemented |
| `--kv-accent-hover` | `accentHover` | Primary hover | Implemented |
| `--kv-accent-active` | `accentActive` | Selected/menu text, pressed accent | Implemented |
| `--kv-accent-subtle` | `accentSubtle` | Selected rows, menu hover, notices | Implemented |
| `--kv-accent-muted` | `accentMuted` | Selection background, primary disabled | Implemented |
| `--kv-accent-border` | Proposed `accentBorder` | Selection/focus border variants | Proposed |
| `--kv-focus-border` | `focus` | Focus border/splitter hover | Implemented |
| `--kv-focus-ring` | No QSS ring equivalent | Future custom painting if needed | Proposed |
| `--kv-positive` | `positive` | Staged diff scope | Implemented |
| `--kv-warning` | `warning` | Warning states | Implemented field |
| `--kv-danger` | `danger` | Errors, destructive hover, diff removal | Implemented |
| `--kv-info` | `info` | Info states | Implemented field |

Known drift:

- KDS light `--kv-accent` is `#5167c4`; current native light `accent` is
  `#5968DF`. This should remain an audit finding until QA confirms whether CM
  keeps the native value for cross-platform contrast or aligns to KDS.
- KDS dark text main maps to `#f2f4f7`; current native dark `textMain` is
  `#CDD2D8`. Treat this as Pending QA, not a blessed baseline.

## Typography

| KDS token | Current native behavior | Status |
| --- | --- | --- |
| `--kv-font-sans` | Inter is loaded from app resources; fallback family is `Inter`. | Implemented |
| `--kv-font-mono` | Uses `QFontDatabase::systemFont(QFontDatabase::FixedFont)`. | Implemented native adaptation |
| `--kv-text-2xs` `10px` | Terminal heading and settings eyebrow use 10px. | Observed |
| `--kv-text-xs` `11px` | Preview hint, status, path labels use 11px. | Observed |
| `--kv-text-sm` `12px` | Tree, checkbox, field labels use 12px. | Observed |
| `--kv-text-md` `13px` | Diff file and editor default nearby. | Observed |
| `--kv-text-base` `14px` | Interface default font size and preview default. | Implemented |
| `--kv-text-xl` `22px` | Settings title uses 22px. | Observed drift from KDS 18px `--kv-text-xl`; Pending QA |
| `--kv-leading-tight/normal/relaxed` | Document-like line heights are explicit pixels; interface controls stay automatic. | Implemented native adaptation |
| `--kv-weight-normal/medium/semibold/bold` | QSS uses 400, 500, 600/650, 700 in selected roles. | Observed |

Rules:

- Interface font size remains bounded to 8-24px.
- Editor/terminal/preview line heights remain at least their font size.
- Control line height remains automatic; geometry comes from density metrics.

## Metrics and QSS destinations

| KDS token | Pixel equivalent at 16px root | Current native destination | Status |
| --- | ---: | --- | --- |
| `--kv-space-1` | 4 | Small margins, menu separators approximate 5px | Observed |
| `--kv-space-2` | 8 | Tooltip padding 6/8, notices 8/12 | Observed |
| `--kv-space-3` | 12 | Button horizontal padding, settings nav padding | Observed |
| `--kv-space-4` | 16 | Several layout gaps in widget code, not globally tokenized | Pending QA |
| `--kv-control-height-xs` | 26 | Diff scope 18px currently smaller | Pending QA |
| `--kv-control-height-sm` | 30 | Status/compact controls 22-26px currently smaller | Pending QA |
| `--kv-control-height-md` | 34 | Generic controls use min-height 32px | Pending QA |
| `--kv-control-height-lg` | 40 | Tabs use 40px min-height | Observed |
| `--kv-sidebar-item-height` | 30 | Tree row 26px, settings nav 34px | Pending QA |
| `--kv-table-row-height` | 52 | Diff table rows 23px because editor diff is dense | Native adaptation, Pending QA |
| `--kv-radius-xs` | 3 | Scrollbar handle radius 3px | Observed |
| `--kv-radius-sm` | 5 | Menus, buttons, nav items | Implemented |
| `--kv-radius-md` | 7 | Menu border radius 7px | Observed |
| `--kv-radius-lg` | 9 | Native cards use 8px | Pending QA |
| `--kv-radius-app-region` | 0 | Main shell/tab/page regions are unrounded | Implemented |

## Focus, disabled, error, and reduced motion

- Focus: current native QSS uses border-color `focus`. KDS also has a ring
  token; a custom focus ring is proposed only if QA requires stronger visibility.
- Disabled: current primary disabled state uses `accentMuted` and `textMuted`;
  generic disabled controls should map to `interactiveDisabled` once exported.
- Error: current error text maps to `danger`; field-level error border/background
  should map to `--kv-danger-border`/`--kv-danger-bg` in a later component slice.
- Reduced motion: KDS reduces durations to near-zero under
  `prefers-reduced-motion`. Native CM currently has no exported motion token.
  Proposed native token snapshots should include effective reduced-motion state
  before adding animations.

## QML

KetPlus CM currently uses Qt Widgets, not QML. If a QML surface is added later,
it should consume the same `NativeDesignTokens` snapshot rather than reading CSS
tokens directly or inventing a second palette.
