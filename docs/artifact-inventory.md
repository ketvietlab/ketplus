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

The Linux assembler emits these sidecars after its install and preliminary
three-package SBOM, before building the deb/AppImage. It checks that the installed
daemon's short revision matches the full packaging source commit. The existing
`SBOM.spdx.json` inside the package remains preliminary; the detailed staged SBOM
is external evidence, not a claim of completed release licensing. Final package
metadata includes the full source identity. `ARTIFACT-EVIDENCE.json` binds the
actual package digests, build metadata, detailed inventory and staged SBOM.
`SHA256SUMS` covers all generated sidecars and packages. No signing/upload occurs.

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
  --evidence "$EVIDENCE/ARTIFACT-EVIDENCE.json" --artifacts-dir "$EVIDENCE" \
  --sbom-out "$EVIDENCE/REVIEWED-SBOM.spdx.json"
```

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

Before release, compare each final archive's extracted payload with the reviewed
stage (including package wrapper files and the AppImage runtime outside that
stage), inspect native dependency closure, verify public corresponding-source
availability, and complete platform builds/smokes, signing/notarization and legal
review. Embedded/static inventory and Qt/Chromium transitive notices remain
mandatory. About currently exposes product/version but still lacks source revision
and notice access. These gaps keep PS-05 open; an inventory test or engineering
package is not artifact acceptance.

Offline checks: `python3 -m unittest discover -s tests/hygiene -p 'test_*.py'`
and `python3 tests/packaging/test_linux_desktop_packages.py`. Fixtures include
real temporary file trees, nested npm packages, native/plugin/font/icon markers,
and failures for absent components/notices/source identity, changed file modes,
bytes, artifacts and symlinks. They require no SDK, signing key or provider quota.
