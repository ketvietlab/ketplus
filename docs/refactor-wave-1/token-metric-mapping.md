# Token and metric mapping

## Source pin and interpretation

- Package: `@ketvietlab/design-system` `0.1.7`
- Repository revision: `6f923d1e13958ce740e50bccab45efc56ccd2f84`
- Source file: `packages/design-system/src/foundations/tokens.css`
- Git blob: `032e5af9eaf27a46a00fab2c8cdac94c4a407c26`
- SHA-256: `f01c77596ac50cfba55d271e2b32fbe90bc60c238affebc46fea5cd4eb8149c1`

The pinned CSS file is the canonical source for KDS values. Native values in
`src/ui/Theme.cpp`, QSS, or widget layout code are observations, never promoted
to canonical KDS truth by this document or a baseline test.

Resolver rules for a future Qt snapshot:

1. Parse the pinned declaration graph and recursively resolve `var()` aliases;
   cycles, missing references, and unsupported syntax are errors rather than
   silent fallbacks.
2. Resolve `light-dark(light, dark)` using the requested native colour scheme,
   including nested aliases before conversion.
3. Convert hex/rgb values to `QColor`. Preserve alpha from forms such as
   `rgb(89 104 223 / 14%)`; do not pre-composite it against a guessed surface.
4. Record source token name and resolved source value separately from any native
   adaptation. The mapping fixture, when implemented, must carry the source
   revision and blob/hash above and fail on unreviewed drift.

## Evidence statuses

| Status | Meaning | Can baseline assert it? |
| --- | --- | --- |
| Canonical source value | Directly resolved from the pinned KDS file | Yes, with provenance fixture |
| Observed native value | Value currently found in public Qt code | Yes, as a current-native regression only |
| Intentional adaptation | Deliberate Qt/platform adjustment with rationale | Yes, once the adaptation is reviewed |
| Unresolved drift | Source and native differ without approval | Assert the difference in a drift fixture; do not bless either as parity |
| QA approved | Native measurement accepted by Integration/QA with recorded evidence | Yes, against that approved evidence |

No item in this Wave 1 document has native visual-measurement QA approval.

## Colour examples

| KDS role | Canonical KDS 0.1.7 (light / dark) | Observed native | Classification |
| --- | --- | --- | --- |
| `--kv-page-bg` | `#f7f5f5` / `#1b1f24` | `#F7F5F5` / `#1B1F24` | Equivalent observed value; source fixture not implemented |
| `--kv-panel-bg` | `#ffffff` / `#1d2228` | `#FFFFFF` / `#1D2228` | Equivalent observed value; source fixture not implemented |
| `--kv-text-main` | `#24262a` / `#f2f4f7` | `#24262A` / `#CDD2D8` | Dark unresolved drift |
| `--kv-accent` | `#5167c4` / `#5968df` | `#5968DF` / `#5968DF` | Light unresolved drift |
| `--kv-panel-border` | `#e9e7e8` / `rgb(255 255 255 / 5.5%)` | Concrete native border fields | Mapping/alpha parity unresolved |
| `--kv-focus-ring` | shadow using `#dde2f7` / `rgb(89 104 223 / 16%)` | Border-only focus in current QSS | Intentional mechanism candidate, pending QA |

The same classification process applies to sidebar, surface, text, interactive,
accent-state, positive, warning, danger, and info roles. Absence from the example
table does not imply parity or approval.

## Typography

At the canonical CSS root baseline of 16 CSS px:

| KDS token | Canonical value | 16px-root equivalent | Current native observation | Classification |
| --- | ---: | ---: | --- | --- |
| `--kv-text-2xs` | `0.625rem` | 10px | 10px in compact labels | Observed native value |
| `--kv-text-xs` | `0.6875rem` | 11px | 11px in hints/status | Observed native value |
| `--kv-text-sm` | `0.75rem` | 12px | 12px in trees/fields | Observed native value |
| `--kv-text-md` | `0.8125rem` | 13px | 13px near editor/diff defaults | Observed native value |
| `--kv-text-base` | `0.875rem` | 14px | 14px interface default | Observed native value |
| `--kv-text-lg` | `1rem` | 16px | No contract claim | Unmapped |
| `--kv-text-xl` | `1.125rem` | **18px** | No 18px mapping established | Unmapped |
| `--kv-text-2xl` | `1.375rem` | **22px** | Settings title uses 22px | Observed match candidate, pending mapping review |

`--kv-font-sans` is a CSS fallback list; current Widgets load Inter and use a
native fallback. `--kv-font-mono` likewise resolves to the platform fixed font
for Widgets. These are platform adaptations, not proof that the selected font
face or metrics are pixel-identical. KDS leading values are unitless multipliers;
current document-like panes often store pixel line heights, while general
Widgets use native layout/font metrics.

## Units, scaling, density, and rounding

- Canonical `rem` conversion starts from a 16 CSS px baseline. The source value
  stays in rem; the table's px number is a reference conversion, not a mandate
  to ignore user font scaling.
- The proposed Qt metric values are logical pixels (`qreal`). Qt applies device
  pixel ratio during rendering; callers must not multiply the logical value by
  DPR again.
- User interface font scaling affects resolved typography. It does not
  implicitly scale spacing/control density unless a separately reviewed density
  policy says so. Platform accessibility/font behavior must remain usable even
  where it prevents exact CSS geometry.
- Preserve fractional logical metrics through layout/style resolution. Round
  only at the final API boundary that requires an integer: use `qRound` for
  nonnegative spacing/radius values and `qCeil` for minimum text/control extents
  to avoid clipping. Tests compare logical values with suitable
  tolerance; they do not compare pre-multiplied device pixels.
- A density variant is an explicit resolver input. It must not be inferred from
  DPR, screen resolution, or platform name.

## Canonical metrics and observed native values

| KDS token | Canonical logical px at 16px root | Observed native use | Classification |
| --- | ---: | --- | --- |
| `--kv-space-1/2/3/4` | 4 / 8 / 12 / 16 | Several approximate margins/gaps | Observed; mapping unresolved |
| `--kv-control-height-xs/sm/md/lg` | 26 / 30 / 34 / 40 | Compact controls 18-26, generic min 32, tabs 40 | Mixed drift; pending QA |
| `--kv-sidebar-item-height` | 30 | Tree 26, settings navigation 34 | Unresolved drift |
| `--kv-table-header-height` | 42 | No reviewed mapping | Unmapped |
| `--kv-table-row-height` | 52 | Dense diff rows about 23 | Intentional adaptation candidate, pending QA |
| `--kv-radius-xs/sm/md/lg` | 3 / 5 / 7 / 9 | Several 3/5/7 values; cards use 8 | Mixed observed/drift |
| `--kv-page-padding-x/y` | 28 / 24 | Settings pages currently use 30 / 26-30 | Unresolved drift |

## Widgets and future QML

Qt Widgets should consume the resolved snapshot through palette, QSS generation,
and layout helpers. A future QML adapter should expose that same snapshot as
value types/singleton data, preserving provenance and logical units. It must not
reparse CSS independently or create a second canonical palette. No QML module or
dependency is required by this documentation slice.

The current offscreen tests are behavioral/native-value regressions. They are
not screenshots, pixel QA, font-rendering evidence, or proof of KDS parity.
