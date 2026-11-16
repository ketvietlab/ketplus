<!-- Target integration, not develop or main. Use an English title/body.
Read CONTRIBUTING.md. Do not include secrets, personal data, user transcripts,
machine-specific paths, or private product/infrastructure details. -->

## Change

<!-- Explain the problem and resulting behavior; link an issue when available. -->

## Validation

<!-- Exact commands/results and platform; UI evidence with synthetic data when
relevant. State checks not run and why. Follow docs/testing.md for your area.
Do not run tests that use provider quota unless explicitly requested. -->

## Review checklist

- [ ] Base branch is `integration`; change stays within module and Lite/Pro boundaries.
- [ ] Relevant tests, formatting, and docs are updated, or exceptions explained above.
- [ ] Diff and new files checked for public safety; `git diff --check` and public-tree guard pass.
