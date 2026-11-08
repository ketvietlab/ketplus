# ADR 0082: Validate update inputs before creating release artifacts

Status: accepted for deterministic archive/notes preflight; transactional
publication, concurrent writers and crash recovery remain open.

The update-preparation script created its archive before checking the release
notes. Missing, unreadable or unsupported notes could strand an archive that
blocked a corrected retry at the same version. A dangling archive symlink passed
the existing existence check; notes destinations could be overwritten. The bundle
version also flowed directly into output paths.

Require a nonempty version starting with an ASCII digit and containing only
letters, digits, dots, plus, underscore and hyphen. Before creating the updates
directory or archive, require readable regular HTML/Markdown notes and refuse
existing archive/notes destinations, including dangling symlinks. Preserve
existing artifacts; an operator can repair invalid input and retry the same
version. Existing valid versions, notes and no-notes invocations remain supported.

The Python fixture redirects fixed Apple utility paths only in a temporary copy
of the script. Synthetic bundles, archive writes and appcast generation exercise
refusal-before-write, outside-target preservation, existing-release preservation
and repair/retry without real keys, signatures, network or publication. The
expanded baseline has 12 failure entries across eight failing methods; the fixed
suite passes all 12 methods. Existing signing CLI fixtures pass 8/8.

This is preflight, not an atomic publish transaction. Concurrent mutation after
the checks, ancestor symlinks, appcast destination handling, key-path
canonicalization and partial failures during ditto/generate_appcast still require
a separate audit. Real Developer ID/notarization/Sparkle signature verification
and update channel/downgrade acceptance are not established by synthetic tools.
No application UI or runtime update behavior changes.
