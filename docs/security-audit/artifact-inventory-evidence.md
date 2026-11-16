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

## Checks and remaining acceptance

The focused temporary-tree tests cover a positive completed review plus failures
for missing components, notices, source materials and source identity; changed
bytes/modes, nested npm identity, artifact/SBOM digests and invalid symlinks.
The existing Linux packaging fixtures pass as well. Public-tree boundary checks
and `git diff --check` are part of the same verification. No C++ files changed.

The local Docker CLI has no available daemon. The native Linux package build,
bundled-browser/Claude adapter and production build-info checks through
`build-desktop.sh`, and fresh SDK-free AppImage/deb smokes were not run. This
exercise supplies no current-branch macOS/Windows/Linux build acceptance, signing,
source-offer availability check or legal review. About source/notice access remains
open as documented in licensing. Reviewer acceptance of tooling is separate from
final per-platform artifact acceptance; PS-05 remains open.
