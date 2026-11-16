# Artifact inventory evidence (T-10)

This records an offline tooling exercise, not acceptance of a release artifact or
closure of PS-05. No program from the inspected package was executed. No signing,
notarization, upload, deployment, provider request or Git history rewrite occurred.

## Actual engineering package

An existing `ketplus-pro_0.1.52_arm64.deb` was copied/extracted into an isolated
temporary directory using the tar data safety filter. Only its installed
`opt/ketplus-pro` tree was collected. Original artifacts remained unchanged.

| Evidence | Value |
| --- | --- |
| Original deb SHA-256 | `0d5265bf83829e2bc808269f19d3a286830a385180e2f82eb95ab09037cdeed7` |
| Inventoried files and symlinks | 6,914 |
| Discovered components | 285 |
| Installed npm components | 109 |
| Native files | 134 |
| Executable files | 31 |
| Node binaries | 1 |
| Plugin/QML classified files | 102 |
| Qt/framework classified files | 75 |
| Images/icons | 11 |
| Symlinks | 51 |
| Detailed inventory SHA-256 | `6c607768975ce2484cd4b3dac59bd38bdb50833f803f43a9172724e3b634cbfc` |
| Staged SPDX SHA-256 | `513adfc3d24a4841b8d5dd41ae61388949fc8148fec7b03c0c7ed429631c9534` |
| Artifact evidence SHA-256 | `ad8a34ecdd01ab4f1a789802ad8362153ae8ac15c93f413f378f85064747e3b7` |

Classification counts overlap. Fonts embedded into native resources cannot be
discovered as standalone font files and require the explicit embedded inventory.
Package wrapper files outside this prefix and the AppImage runtime require their
own final artifact review; this prefix scan makes no claim to cover those bytes.

The old package's build metadata contains only a short source revision. The full
commit was not resolvable in this checkout. Source identity was explicitly recorded
as `NOASSERTION`, rather than inferred from the current branch. Its fresh review
template produced a failing gate with these rule IDs:

- `source-identity-unresolved`
- `artifact-source-identity-mismatch`
- `component-identity-or-license-unresolved`
- `component-notices-missing`
- `component-source-materials-missing`
- `embedded-or-dependency-evidence-missing`

These failures are expected and remain unresolved for that package. Sidecar bytes
and extraction are retained outside the public tree for reviewer reproduction;
the team task records their location. This older package is not a build of T-10.

## Follow-up: complete Debian payload and source/notice candidates

After the first review, the same actual deb was freshly extracted including both
its control and data archives. On this macOS host, a portable ar/zstd/tar reader
with the data safety filter supplied the raw-extraction adapter; this was not a
native `dpkg-deb` or Linux smoke run. No installed/package executable was run.
The complete view includes `DEBIAN/control`, the `/usr` launcher/service/icons and
the product prefix. It contains **6,925 files/symlinks and 298 components**, including
109 npm packages. A prefix-only scan omitted 11 shipped files.

| Follow-up evidence | SHA-256 |
| --- | --- |
| Complete payload inventory | `5e891f94073840116aa2e7462853f94dcd5d624a270159078938ea91966007d7` |
| Complete payload candidate SPDX | `2452fd5c0331dd8bdb3be9b446a60adf93f3419a5a8f03b6669c1efbc5ec374d` |
| Complete artifact binding | `b93b5798b5c522639c2c8af60a59010e16fdd628e59c94a4886ad343d4395f72` |

The recorded short revision was subsequently resolved through the public GitHub
commit lookup to `5b9417abc8b9b9fcf5a20301397eed9e1644ba27`. This resolves the
abbreviation; it is not proof of reproducibility or a current-branch build.
The copied evidence metadata explicitly identifies that resolution and does not
alter the original package or its original metadata. The original prefix evidence
above remains a historical record of the first exercise.

The new candidate mapping has declared source hints for 53 components and notice
paths for 111 components, derived from installed npm metadata/files and known
wrapper provenance. Other native/static/Qt/Chromium identities and corresponding
source/notice materials still require reconciliation. The final-payload gate
recognizes this full Debian layout but remains failed for unresolved component
identities/licenses/source materials and embedded/dependency audit evidence.
No source hint is treated as legal acceptance or publication authorization.

## Checks and remaining acceptance

The follow-up local matrix passed 23 hygiene/inventory/source-identity tests,
9 Linux packaging fixtures and all 28 Lite CTest targets on macOS ARM64 with
Qt 6.11.2. The About/brand target passed 12 Qt cases. One earlier full-suite run
timed out in the existing terminal Ctrl-C case; its isolated rerun and the final
full 28-target run passed without terminal changes. No provider quota was used.

The focused temporary-tree tests cover a positive completed review plus failures
for missing components, notices, source materials and source identity; changed
bytes/modes, nested npm identity, artifact/SBOM digests and invalid symlinks.
The existing Linux packaging fixtures pass as well. Public-tree boundary checks
and `git diff --check` are part of the same verification. No C++ files changed.

Shared About now implements revision/public source and installed notice/license
access. Its Qt tests exercise edition/platform path resolution, keyboard URL actions,
missing files, dirty/unknown revisions and live light/dark at every supported zoom,
including the Lite minimum window size. The body scrolls when necessary instead of
shrinking access controls. Representative Qt captures were inspected. Lite macOS
installation now places notices inside its bundle. A real local `cmake --install`
staging exercise confirmed LICENSE and THIRD_PARTY_NOTICES.md exist inside both the
Lite and Terminal bundles; no deployed app was changed. A source build without Git
reports the source revision as unavailable rather than inventing one.

The local Docker CLI has no available daemon. The native Linux package build,
bundled-browser/Claude adapter and production build-info checks through
`build-desktop.sh`, and fresh SDK-free AppImage/deb smokes were not run. This
exercise supplies no production macOS/Windows/Linux package acceptance, signing,
source-offer availability check or legal review. The local Lite build/suite and
About checks verify implementation, not a redistributable platform artifact.
Reviewer acceptance of tooling is separate from
final per-platform artifact acceptance; PS-05 remains open.
