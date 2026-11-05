# Evidence Validation Contract

The validator checks structure, integrity and declared provenance. It does not
authenticate human consent or independently prove pixel correctness. A reviewer
must verify the approval URL and comparison result before accepting evidence.
No visual reference or API approval exists in this baseline.

## Required Data

`gate-policy.cjs` owns required task/evidence IDs, categories and prerequisite
statuses. Each readiness row must exactly match its task's prerequisite IDs,
reviewer and derived status. All prerequisites are required. A reviewed handoff
opens only the corresponding audit/test slice. Implementation additionally needs
contract freeze and coordinator scope approval. Visual acceptance needs approved
reference and paired pixel evidence with complete state/theme coverage.

`assetHashes` must enumerate every file under the design revision's `assets/`
tree plus the design-system lock. The source is Git commit
`d0591869113ad4420961bff2ef2f2ac0ea840b58`, not the submitted asset array. The
validator checks all local bytes against their manifest digests AND pinned Git
blobs. Removing an entry or changing both asset and digest fails.
Design-bundle artifacts with a `sourceRevision` are hashed from that Git blob;
a live copy that differs is reported as a `drift` warning (decision D18).

Evidence entries require an ID, category (`type`), status, immutable `revision`,
RFC3339 `recordedAt`, and artifact array with unique IDs. Completed/reviewed
records require reviewer, provenance and artifacts. Pending/missing/blocked/
not-run records require a reason. Artifact paths must resolve to regular files
inside the repository after resolving symlinks. Nulls, malformed JSON and I/O
errors produce `{code, path, message}` errors and fail closed.

Each artifact has `id`, `type`, `path` and `sha256` or `sourceRevision`.
`sourceRevision` is verified by reading the Git blob and comparing actual bytes;
it is not an ignored annotation. If both digest and revision exist, both must
match. Git source revisions must be available locally; missing objects fail,
including on shallow CI clones. Offline owner review receipts pin source URLs
and hashes of the exact bytes read, not a claim that those owners approved them.

## Capture And Comparison

Reference/native/diff artifacts must be actual PNG bytes, with `type: screenshot`,
`mediaType: image/png`, `.png` path, SHA-256, unique ID, role and physical dimensions.
The parser checks PNG signature, chunk lengths/order/CRC, decompression and
scanline length/filter bytes. The supported capture interchange format is
8-bit RGB/RGBA, non-interlaced PNG. Unsupported encodings fail explicitly.

Every image carries `capture` metadata: exact atlas state URL, light/dark theme,
1440x900 logical viewport, DPR 2, 2880x1800 physical pixels, revision, timestamp,
fixture artifact/digest, and environment. Environment includes macOS/arm64,
OS version/build, Qt 6.11.1, font family/version/pinned asset digest, renderer,
capture tool/version, web zoom 100% and typography scale 115%. Paired captures
must match shared environment inputs and fixture; reference/browser, native and
diff tools/renderers are recorded separately and may differ.

`visual-reference.status` can become `approved` only with reference screenshots
and an `approval` artifact of type `approval-record`. The JSON approval record
must bind evidence ID, exact revision, reviewer, role `designer/user`, decision
`approved`, timestamp, HTTPS provenance URL and all artifact digests. This record
is auditable provenance, not a validator-generated approval. Contract freeze
records similarly require the owning role in the gate policy.

`native-pixel` uses `referenceEvidenceId` and `referenceRevision` to identify the
approved reference, and `comparisons` records instead of unpaired arrays. Each
comparison identifies reference/native/diff IDs and their SHA-256 values,
state/theme, and a digest-pinned JSON report. The report binds comparison ID,
all three hashes, result, tool/version and timestamp. Pixel evidence also needs
a reviewer approval record with role `design-qa`. Paths must be distinct and
every native/diff artifact paired exactly once. A partial capture can be valid
evidence without opening the global visual gate: that requires all 112 atlas
states in both themes, with matching comparison coverage. No numeric tolerance
or mask is invented or changed by this validator.

## Historical And Test Evidence

Historical `verificationPerformed` and original evidence timestamps/revisions
remain historical snapshots; they are not required to equal a later HEAD.
R00-C adds new owner review receipts with their own timestamps/revisions. New
capture records can pass integrity without rewriting historical `not run`.

`tests/design/integration-synthetic.cjs` creates only test parser fixtures in
temporary directories. Generated images, capture metadata and approval records
are marked `testOnly`; normal checker invocation rejects them. Tests explicitly
opt in with `validate({testOnly: true})`. No generated test image or approval
record is committed as product evidence or accepted as a visual baseline.

## Commands

From the repository root, with pinned Git history available:

```sh
git fetch --no-tags origin 37a14562a11b6ec9fb7f137e9e93c95af464bc8d
node tasks/desktop-refactor/wave-1/integration/check-integration.cjs
node --test tests/design/integration-evidence.test.cjs
node tasks/desktop-refactor/check-plan.cjs
git diff --check
```

The fetch obtains the exact pre-squash PR #17 source used by historical artifact
references. It does not cherry-pick or copy design history. After an intentional
manifest/policy edit, generate Markdown with
`node tasks/desktop-refactor/wave-1/integration/render-readiness.cjs`; CI checks
parity without writing generated output. The generator first validates the
manifest; it cannot turn an invalid READY claim into a passing matrix.
