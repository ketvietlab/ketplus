# Managed browser identity enrollment

Status: preparation only. PS-04 remains open. The existing production installer
and launch paths have not been connected to the primitives described here.
No production browser digest or signing identity has been enrolled. This document
does not establish browser readiness, runtime acceptance or release approval.

## Policy and current gap

The selected policy retains automatic managed-browser downloads and requires an
independent trusted digest or signature, including verification on cache reuse
(release-disposition D-04; team decision D-3, reaffirmed by D-8). A platform without
independent publisher evidence remains not ready. Project enrollment authority
is not an approved substitute for that evidence. HTTPS, a download URL, a revision,
`INSTALLATION_COMPLETE`, and a cache-local hash receipt are not identity roots.

The pinned `playwright-core` 1.63.0 registry identifies Chrome for Testing
153.0.8010.12, revision 1243. It supplies download URLs but no browser digest or
signature. The official [CfT metadata documentation](https://github.com/GoogleChromeLabs/chrome-for-testing/blob/main/README.md)
describes version/download URL endpoints. Its upstream
[checksum request](https://github.com/GoogleChromeLabs/chrome-for-testing/issues/70)
remains open at the time of this preparation. These observations do not prove
that no independent identity source exists; no usable source for the complete
pinned matrix has been established in this task.

The [bounded source review and proposed enrollment route](engine-provenance-research.md)
records exact-version metadata, platform-specific signing gaps and the separate
responsibilities for artifact authentication, manifest signing and revocation.
Neither that review nor its distribution fixture enrolls a production identity.
The current preparation follows [Route A: an immutable manifest bundled with an
authenticated application release](engine-release-manifest.md). Its app-release
trust chain and initial artifact enrollment remain unverified prerequisites.

| Playwright platform | Browser version / revision | Enrolled independent identity | Runtime acceptance |
| --- | --- | --- | --- |
| macOS ARM64 | 153.0.8010.12 / 1243 | Missing | Not run |
| macOS x64 | 153.0.8010.12 / 1243 | Missing | Not run |
| Linux x64 | 153.0.8010.12 / 1243 | Missing | Not run |
| Linux ARM64 | 153.0.8010.12 / 1243 | Missing | Not run |
| Windows x64 | 153.0.8010.12 / 1243 | Missing | Not run |

This lists the pinned registry's managed-engine mappings, not a new product
support promise. Browser runtime maintainers own enrollment and updates; a
security reviewer must verify the evidence behind every published identity.
Unknown versions/platforms must produce an explicit readiness error rather than
falling back to an unverified download or cache. The missing identities are an
implementation blocker, not a decision to switch to installed-only operation.

## Prepared verification boundary

`src/engine-integrity.mjs` accepts a reviewed identity manifest with an exact
Playwright version, browser revision, browser version and platform/architecture
match. Each platform entry contains an archive HTTPS URL, a relative executable
path, an archive SHA-256, an installed-tree SHA-256 and enrollment evidence
(`provenance.url` plus an immutable source revision). Evidence fields record a
review; their presence alone does not authenticate it. The caller must load this
manifest from the application, never from tool input, environment or the cache.

The archive reader opens nonblocking, validates a bounded regular descriptor,
checks SHA-256 and returns the exact authenticated bytes. Extraction must consume
those bytes rather than reopen a path that can be replaced after verification.
The tree hash covers every relative entry, directory, file content, executable
permission and symbolic-link target. Links outside the tree and non-regular
entries fail. A marker or receipt added inside the tree also changes the hash.
Files are read through validated descriptors; a FIFO substituted after a path
precheck fails without waiting for a writer.

The tree measurement is SHA-256 of newline-terminated JSON array records in
depth-first traversal, with each directory's names sorted using JavaScript's
default string order. Records are `[path,"directory"]`,
`[path,"link",target]`, or
`[path,"file",byteLength,hasExecuteBit,contentSha256]`. Paths use `/`.
Ownership, timestamps and non-executable permission bits do not enter this
content identity. Platform filesystem semantics need native validation before a
platform's tree hash can be published.

`src/engine-store.mjs` prepares a lifecycle around that identity:

1. Probe only a complete content-addressed tree and verify all entries before
   returning an executable. Cache-local metadata never grants trust.
2. Download into a new private stage, authenticate the archive bytes, extract
   into that stage, verify the tree and executable, then publish by rename.
3. Ignore abandoned stages during discovery and clean failed stages from the
   current attempt. A later attempt can recover without executing partial data.
4. On concurrent publication, verify the winning tree. A rename collision is not
   evidence of identity. A corrupt published cache fails explicitly.

`src/engine-release.mjs` prepares release/package/host-bound inventory selection
and an integration seam for this store and a launcher. It checks every declared
browser flavor/helper before side effects, applies bundled revocations, and
rechecks the selected trees before its launch callback. The callback is fake in
tests; production launch paths and implicit Playwright helpers are not wired.

On POSIX, the prepared store also requires a real cache directory owned by the
current user with no group/other permissions. Windows ACL validation remains
unimplemented. These checks address persistent cache substitution, not a hostile
process with the same user identity continuously racing writes after verification.
No file-reader check alone provides that operating-system isolation.

The network and ZIP adapters are deliberately not supplied yet. A production
adapter must constrain redirects to enrolled HTTPS origins, preserve required
proxy operation without weakening TLS, enforce archive expansion/path limits,
and use the authenticated bytes in a private stage. No synthetic test archive is
a production browser artifact.

## Enrollment and integration still required

Before publishing any production identity:

1. Obtain expected artifact identity through an independently authenticated,
   reviewable source. Preserve the signing root or immutable metadata revision,
   signer verification result and exact platform/version/artifact association.
   A digest calculated from the same newly downloaded archive is insufficient.
2. Authenticate the archive against that source before deriving the installed
   tree hash in a clean, bounded extraction. Record extraction/permission
   normalization per platform and retain the source evidence for review.
3. Publish the manifest with the matching Playwright lockfile change and have a
   second reviewer verify identity enrollment. A missing entry must fail closed.
4. Wire production discovery, download, extraction and startup through the store;
   retain automatic downloads for enrolled identities. Reconcile development
   `playwright` cache selection and embedder launch overrides so they cannot
   bypass managed identity checks. Keep read-only readiness free of installs.
5. Exercise the real network/ZIP adapters and production launch boundary with
   offline fixtures. Verify interrupted install recovery, replaced archive/cache,
   unknown identity, host/environment overrides and supported platform behavior.
6. Run the repository's affected test matrix and obtain reviewer acceptance.
   Document runtime, packaging and release evidence separately.

## Evidence and limits

The preparation tests use fixed synthetic archive/tree snapshots. They cover
identity mismatch/missing pins, archive substitution, exact-byte handoff, a
FIFO-after-precheck watchdog, injected loaders/resources, cache receipts,
symlinks, non-regular entries, owner-only POSIX caches, interrupted attempts,
abandoned stages and concurrent publication. They do not execute a fake or real
browser. The adapters are fixtures rather than a real ZIP parser or downloader.

Run these offline tests with:

```sh
cd apps/browser
node --test test/engine-integrity.test.mjs test/engine-store.test.mjs test/engine-release.test.mjs test/engine-distribution.test.mjs
```

Only macOS ARM64 preparation checks have run so far. No Windows/Linux runtime,
real browser, provider, production install, packaging or deployment has been
verified. The full browser wiring matrix remains required after integration.
