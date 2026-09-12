# Finding resolution and compatibility risks

## Review finding resolution

| Finding | Resolution in R01-B | Remaining gate |
| --- | --- | --- |
| Worktree observations were treated as universal blockers | Observation and per-operation policy are separate; open/select allows current/main, dirty, untracked, and locked states with truthful warnings | API approval and runtime tests |
| Repository identity used path-shape assumptions | Proposal uses canonical Git common-dir plus registered git-dir; linked worktrees may live anywhere | Cross-platform implementation evidence |
| Worktree lifecycle and TOCTOU were underspecified | Canonicalization, case/symlink rules, detached/unborn, missing/prunable/bare, reuse, thread/ownership/correlation/timeout/cancellation/exactly-once outcome, and non-atomic revalidation limit are documented | API approval |
| Settings host embedded namespace policy | Only existing built-in ids `general` and `appearance` are reserved; no prefixes; consumer ids are opaque | API approval |
| Descriptor/page identity and ownership were ambiguous | Descriptor is post-registration source of truth; mismatch rejects; ownership, re-registration, destruction, connection lifetime, results, tie order, and selection behavior are explicit | API approval |
| Proposed dirty-only Save was described as compatible | Current apply-all behavior is baseline-tested; dirty-only is called out as a separate migration/change | Migration decision |
| Reset/Cancel/preview/failure semantics were open | Reset is dialog-wide and buffered; reject paths do not apply; only ThemeManager preview has guaranteed rollback; `void apply()` is non-transactional | Future result/hook API only if required |
| `--kv-text-xl` mapping was wrong | Corrected to 18px; 22px is `--kv-text-2xl` at 16px root | None for source fact |
| Native values were at risk of becoming KDS truth | Provenance, resolver rules, status categories, units/scaling/rounding, drift and QA gates are separated | Mapping fixture and QA |
| Parser/palette tests overstated enforcement/parity | Renamed as parser observation/current-native baselines; current SettingsDialog behavior gets isolated synthetic storage coverage | Proposed API tests remain planned |
| Installable SDK was presented as a release blocker | Kept as an optional independent scope; build-tree `KetPlusCM::*` consumers remain supported | Separate SDK decision |

## Compatibility risks

| Risk | Impact | Mitigation / decision |
| --- | --- | --- |
| Headers are consumed from build-tree `src/` paths | Moving headers may break consumers | Do not couple runtime slices to install/export work; scope SDK hardening separately |
| `ThemePalette` contains concrete values | Consumers may mistake native observations for canonical tokens | Retain it during migration; add source-pinned snapshot and drift fixture before claiming mapping |
| QSS role names are de facto integration points | Renames can alter downstream composition | Stabilize only reviewed roles; do not infer public status from a hard-coded baseline |
| Current `addPage` accepts raw pointers and duplicate ids | Composition and ownership can be brittle | New typed registration result and ownership rules; retain old path as an adapter |
| Current Save applies clean and dirty pages | Dirty-only conversion can remove relied-on calls | Baseline apply-all now; require explicit compatibility decision before changing it |
| Extension `apply()` is `void` and stores are independent | Validation/partial persistence cannot be represented atomically | Make no transaction guarantee; design a result API only if a concrete need is approved |
| Extension preview has no rollback hook | Cancel cannot guarantee reversal of external preview side effects | Guarantee rollback only for existing ThemeManager; keep extension previews local for now |
| Worktree state can change after checks | Operation may act on stale identity/state | Revalidate identity immediately before use, scope commands, return truthful failures; do not claim atomicity |
| Dirty/locked policy differs by operation | A universal blocker would prevent safe selection and risk UI data loss | Preserve observation facts; map them per operation and preserve tabs/unsaved work |
| Filesystem case/symlink behavior varies | String comparisons can select the wrong registration | Use canonical Git identity and filesystem-aware comparison, with platform tests |
| Visual native values differ from KDS | Accidental drift could be blessed | Keep native measurements Pending QA and source mapping independently verifiable |

Deferred generic Git behavior remains eligible for public CM ownership when it
receives its own scope. Product policy belongs in its consumer and is not encoded
in this public contract.
