# Staged artifact inventory and review gate

`packaging/artifact_inventory.py` reads an actual `cmake --install` tree or an
isolated extracted engineering package. It never executes staged programs or
loads libraries. Run it separately for each edition, OS and architecture:

```sh
python3 packaging/artifact_inventory.py --stage "$STAGE" --out "$EVIDENCE" \
  --source-commit "$FULL_SOURCE_COMMIT" --platform macos-arm64 --epoch "$SOURCE_DATE_EPOCH"
```

The stage must be quiescent and the output must be outside it. Existing evidence
files are refused. Use `NOASSERTION` when an old artifact lacks a verifiable full
source identity; this permits inventory collection and always fails the review
gate. A short revision alone is insufficient. Do not infer provenance from a
nearby checkout, an artifact filename or a successful build.

The output contains:

- `STAGED-INVENTORY.json`: every regular file's SHA-256, size, mode and relative
  path, internal relative symlink targets/checksums, directory modes, source identity
  and platform. Unreadable directories, special files, dangling/cyclic links and
  links outside staging fail collection. Symlink directories are never followed.
- `STAGED-SBOM.spdx.json`: a candidate SPDX 2.3 file/component inventory. Native
  magic, executables, Node, Qt/framework names, plugin/QML paths, fonts and images
  are classified from staging. Every installed npm root manifest, including nested
  and scoped packages, becomes a component. Other resources remain inventoried.
  No version or license conclusion is inferred from a binary's filename.
- `COMPONENT-REVIEW.template.json`: an intentionally incomplete review input.
  It retains all discovered components and file membership. Reviewers must resolve
  each component's name/version, license, source identity, installed notices and
  corresponding-source/offer materials. Notice/source paths must identify nonempty
  regular files inside the inventoried tree. Their bytes are bound by the inventory.
  Installed npm manifests supply declared license/public repository hints and their
  own notice/source file paths. These are declarations, not verified license or
  source-availability conclusions. Credential-bearing and unknown-host repository
  URLs are not copied into the hints.

For stronger npm provenance, collect the exact published tarballs named by the
matching source revision's production lockfiles. Check the HTTPS registry origin;
save each archive as `SHA256(resolved URL).tgz` in an external archive directory.
The offline comparator verifies the lock's SHA-512 before reading bounded tar
members, without extracting or executing them:

```sh
python3 packaging/npm_payload_evidence.py --stage "$STAGE" \
  --inventory "$EVIDENCE/STAGED-INVENTORY.json" --archives "$NPM_ARCHIVES" \
  --lock "opt/ketplus-pro/lib/ketplus/ketplus-browser=$SOURCE/apps/browser/package-lock.json" \
  --lock "opt/ketplus-pro/lib/ketplus/claude-sidecar=$SOURCE/apps/daemon/providers/claude-sidecar/package-lock.json" \
  --out "$EVIDENCE/NPM-PAYLOAD-EVIDENCE.json"
```

Lock roots are relative to the actual extracted stage (use `rootfs/usr/...` for
AppImage). Each installed component retains ownership of its own files, including
nested packages. Missing archives, lock/manifest inconsistencies and differing
files produce a nonzero exit. `allNpmFilesMatch` establishes byte correspondence,
not license sufficiency, SDK redistribution permission or dependency-audit approval.
Reports contain public registry URLs/digests and relative paths; rejected URL
credentials and parser input are not echoed. Network collection is separate from
this offline tool and its fixture tests.

Linux packaging supplements the MIT notice omitted from the published
`standardwebhooks@1.1.1` archive using the exact upstream **library** license in
`packaging/licenses/standardwebhooks-MIT.txt`. Its source commit, package manifest
digest and notice digest are pinned in `packaging/linux/npm-notice-supplements.json`.
Package changes require re-review; a different existing LICENSE is never overwritten.
The notice is installed inside that package before assembly, so the final payload
inventory and npm notice mapping cover it. This fixes future assembly; it does not
change or retroactively accept an older package. The upstream specification's root
Apache license is not substituted for the JavaScript library's MIT license.

The Linux assembler first completes both packages, then extracts each final
artifact without executing it. `dpkg-deb --raw-extract` includes control metadata,
maintainer scripts and all data, including wrappers outside `/opt/ketplus-pro`.
AppImage collection uses `unsquashfs` at the verified runtime offset and preserves
the actual binary prefix as `runtime-prefix`, alongside the complete `rootfs/`
(AppRun, desktop entries, icons and installed product). A prefix-only inventory
cannot pass the Linux distribution gate.
The extractor sets a child-only umask of zero for unsquashfs so non-root extraction
does not mask ordinary recorded file modes. The runtime-prefix file inherits the
AppImage artifact's mode. Inventory modes describe the extracted filesystem;
archive ownership, timestamps, xattrs and cross-platform symlink-mode semantics
require separate native verification. They are not established by this scanner.

Each format gets its own `deb-evidence/` or `appimage-evidence/` directory containing
inventory, candidate SPDX, review template and `ARTIFACT-EVIDENCE.json` with its
`payloadLayout`. Bindings use fenced paths relative to the outer artifact output
directory, allowing independent evidence for the two different payloads. Known
recipe wrappers reference their exact public source commit and installed MPL text.
The AppImage prefix references its verified upstream source/notice pin; its static
dependency license/source obligations remain unresolved until reviewed.

The assembler checks that the installed daemon's short revision matches the full
packaging source commit. The older `SBOM.spdx.json` inside the package remains
preliminary; detailed final-payload SPDX is external evidence. `SHA256SUMS` covers
all generated sidecars and packages recursively. No signing/upload occurs.

## Review input and fail-closed check

Copy the template to a review file. Preserve each discovered component's ID,
name, known npm version and complete file list. Add separate entries for static
libraries, embedded fonts/icons/configuration and native transitive components;
their `files` can overlap the executable or resource containing them. Set
`embedded: true` for a component with no separate installed file. Resolve every
`NOASSERTION`, supply `notices` and `sourceMaterials` as lists of installed paths,
and include the source revision/version required by that component's contract.

Set `embeddedInventoryComplete` and `dependencyAuditComplete` only after reviewing
the matching build inputs and platform dependency closure. `buildEvidence` and
`dependencyEvidence` must name nonempty installed evidence files. These explicit
attestations require human review: scanning opaque binaries cannot establish
their complete static composition or whether an offer satisfies a license.
The build evidence must reconcile the claimed full source revision with the
installed executable, including all vendored/static dependencies. Rebuild and
regenerate evidence if notices, source materials or audit files must be added.

Set `artifactEvidenceSha256` to the SHA-256 of the canonical
`ARTIFACT-EVIDENCE.json` bytes emitted by the assembler, then run:

```sh
python3 packaging/artifact_gate.py --stage "$STAGE" \
  --inventory "$EVIDENCE/STAGED-INVENTORY.json" --review "$REVIEW" \
  --evidence "$EVIDENCE/ARTIFACT-EVIDENCE.json" --artifacts-dir "$ARTIFACTS" \
  --sbom-out "$EVIDENCE/REVIEWED-SBOM.spdx.json"
```

For Linux, `STAGE` is the corresponding extracted complete payload,
`EVIDENCE` is the format's evidence directory and `ARTIFACTS` is the outer output
directory. For standalone collection they can use the original external-evidence
layout with `ARTIFACTS=EVIDENCE`.

The check rescans staging, verifies inventory/SBOM/artifact digests and source
metadata, requires coverage of every discovered file/component, and refuses
missing notices, source materials or audit attestations. Exit 1 means incomplete
or changed evidence. Diagnostics use rule IDs and never echo input contents.
An exit 0 produces `inventoryLicenseGatePassed: true`; `publicationAuthorized`
remains false. On success, `--sbom-out` writes a new external SPDX file containing
every reviewed component, including added embedded/static components, its supplied
license/source conclusions and notice/source-material references. The gate result
binds its SHA-256 as `reviewedSbomSha256`; existing output is never overwritten.
Keep the gate result with the reviewed SBOM and review input.
This is a completeness/integrity check on the supplied review,
not a legal sufficiency decision or independent proof that its attestations hold.

For non-Linux collectors, use `bind_artifacts()` from the inventory module after
assembling immutable artifacts and `BUILD-METADATA.json` with `sourceCommit`.
Write its returned object using `encoded()`. Keep inventory and SBOM bytes
unchanged beside the artifacts. Review and gate outputs remain external to staging
to avoid self-referential checksums. Final signing changes bytes and requires a new
artifact binding and review; unsigned evidence cannot authorize signed downloads.

## Remaining acceptance

Before release, independently reproduce the Linux final-payload extraction (or
collect the complete distribution payload on other platforms), inspect native
dependency closure, verify public corresponding-source
availability, and complete platform builds/smokes, signing/notarization and legal
review. Embedded/static inventory and Qt/Chromium transitive notices remain
mandatory. Shared About exposes the build-time full Git revision, corresponding
public source and installed notices/license files. Dirty builds are labelled and
link only the base commit; source archives without Git show an unavailable revision
and disable that link. Missing installed files disable their actions and show a
clear message. macOS resolves bundle Resources/licenses; Linux/Windows installs
resolve their edition's share/licenses directory or a packaged licenses directory.
Final source/notice and per-platform artifact gaps keep PS-05 open; a test or engineering
package is not artifact acceptance.

Offline checks: `python3 -m unittest discover -s tests/hygiene -p 'test_*.py'`
and `python3 tests/packaging/test_linux_desktop_packages.py`. Fixtures include
real temporary file trees, nested npm packages, native/plugin/font/icon markers,
and failures for absent components/notices/source identity, changed file modes,
bytes, artifacts and symlinks. They require no SDK, signing key or provider quota.
