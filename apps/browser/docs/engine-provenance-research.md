# Managed engine enrollment route: source review

Reviewed 2026-11-16 for `playwright-core` 1.63.0, Chromium registry revision 1243,
Chrome for Testing (CfT) 153.0.8010.12. This is a bounded metadata/source review,
not artifact authentication. No browser archive was downloaded, extracted or
executed. No production hash, signing key or trust root was created or enrolled.
Production wiring remains blocked; the selected managed-download policy stands.

## Sources checked

| Source | Observed result | Identity limit |
| --- | --- | --- |
| [Exact CfT version catalog](https://googlechromelabs.github.io/chrome-for-testing/153.0.8010.12.json) | Version 153.0.8010.12, Chromium revision 1681091; `chrome` URLs include all five registry platform mappings below | Version/URL metadata has no archive SHA-256, signature or attestation fields |
| [Pinned Playwright registry](https://github.com/microsoft/playwright/blob/v1.63.0/packages/playwright-core/src/server/registry/index.ts), [revision descriptor](https://github.com/microsoft/playwright/blob/v1.63.0/packages/playwright-core/browsers.json) | Revision 1243 selects this browser version and `cdn.playwright.dev/builds/cft/` paths | Registry revision 1243 and CfT revision 1681091 are different namespaces; neither authenticates archive bytes |
| Google Storage object metadata for each exact `chrome` ZIP | Selected fields returned name, size, generation, MD5 and CRC32C; custom `metadata` was absent | No independently authenticated SHA-256/signature in the inspected fields. Generation, transport, ETag, MD5/CRC and another mirror are not adopted as authenticity roots |
| [Playwright download implementation](https://github.com/microsoft/playwright/blob/v1.63.0/packages/playwright-core/src/server/registry/oopDownloadBrowserMain.ts) | Checks transfer length, extracts, adjusts executable permissions and writes a completion marker | No publisher-authenticated archive identity check in this path |
| [Release asset API](https://api.github.com/repos/microsoft/playwright/releases/tags/v1.63.0) | `assets: []` | No attached browser checksum/signature/attestation artifact there; this does not establish absence everywhere |
| [npm attestation endpoint](https://registry.npmjs.org/-/npm/v1/attestations/playwright-core@1.63.0), [publish workflow](https://github.com/microsoft/playwright/blob/v1.63.0/.github/workflows/publish_release.yml) | Decoded attestation subjects name `pkg:npm/playwright-core@1.63.0` and its package digest | The separately fetched browser ZIP is not a subject. Sigstore signatures were not verified in this review; decoding a subject is not attestation validation |

The object-metadata request template was
`https://storage.googleapis.com/storage/v1/b/chrome-for-testing-public/o/<URL-encoded-object>?fields=name,size,generation,md5Hash,crc32c,metadata`,
where the object is `153.0.8010.12/<platform>/chrome-<platform>.zip`.
Requests were size/time bounded and retained normal TLS verification. The Python
client's missing local CA chain prevented its initial requests; system `curl`
succeeded without disabling TLS checks. A guessed GitHub `main/data/<version>.json`
path returned 404; the official exact-version catalog above was available.

The open [CfT checksum request](https://github.com/GoogleChromeLabs/chrome-for-testing/issues/70)
concerns ChromeDriver, not proof of every Chrome archive's signing state. The
[Playwright integrity request](https://github.com/microsoft/playwright/issues/43981)
was closed with advice about installed browsers and locally computed hashes;
it supplies no authenticated expected digest for this exact version. Neither
issue's existence/status is treated as a production trust root.

## Platform and code-signing evidence

| Runtime mapping / official CfT platform | Distribution association | Signing evidence and remaining gap |
| --- | --- | --- |
| macOS ARM64 / `mac-arm64` | Exact catalog and registry both name `chrome-mac-arm64.zip` | Exact-version [Chromium signing configuration source](https://chromium.googlesource.com/chromium/src/+/refs/tags/153.0.8010.12/chrome/installer/mac/signing/config.py) defines signing machinery, not evidence that the distributed app has a valid publisher signature. The exact bundle/resource envelope was not inspected |
| macOS x64 / `mac-x64` | Both name `chrome-mac-x64.zip` | Same source-level limit; no exact artifact Developer ID, Team ID, chain or resource-seal verification is established |
| Linux x64 / `linux64` | Both name `chrome-linux64.zip` | No detached signature or signed ZIP digest located in the inspected metadata; ELF/archive bytes were not inspected. [Google's Linux package signing keys](https://www.google.com/linuxrepositories/) concern package-manager artifacts and cannot be assigned to this ZIP without a signed binding |
| Linux ARM64 / `linux-arm64` | Both name `chrome-linux-arm64.zip` under the CfT version namespace | This exact version is catalogued as CfT, rather than inferred from a legacy Chromium ARM64 build. No authenticated digest/signature binding was established; byte equality between Google and Playwright copies was not measured |
| Windows x64 / `win64` | Both name `chrome-win64.zip` | A [CfT maintainer statement](https://github.com/GoogleChromeLabs/chrome-for-testing/issues/205) describes unsigned binaries in an earlier Windows issue. It is contextual evidence, not an Authenticode inspection of 153.0.8010.12. No exact EXE/DLL signer or chain has been established |

All five rows remain unenrolled. No platform is silently removed from support.
Matching archive names and catalog entries do not prove identical archive bytes.
The other-version macOS [resource-envelope report](https://github.com/GoogleChromeLabs/chrome-for-testing/issues/227)
is an unverified issue report and is not generalized to this exact version.

If a publisher-signature route becomes available, macOS enrollment must validate
the intended identity and whole resource/nested-code envelope using the policy
described in [Apple TN2206](https://developer.apple.com/library/archive/technotes/tn2206/_index.html).
An ad-hoc CodeDirectory or main-executable-only check is insufficient. A Windows
route needs [Authenticode verification](https://learn.microsoft.com/en-us/windows/win32/api/wintrust/nf-wintrust-winverifytrust),
an independently enrolled publisher identity, and coverage for unsigned sidecars
and resources. A valid signature on one executable does not authenticate the ZIP
or every DLL. These are future enrollment requirements, not completed checks.

## Proposed project-controlled release manifest

This route is a proposal for reviewer evaluation. It does not authorize key
creation, signing, artifact enrollment or a weaker interpretation of authenticity.

1. **Authenticate the initial artifact.** A designated browser-runtime release
   maintainer obtains a publisher-signed digest/attestation or a documented,
   independently authenticated statement from the upstream release authority
   binding the exact artifact bytes, version and platform. A separate security
   reviewer verifies the authority, signature/statement, scope and retained
   evidence. Then verify the archive and derive the installed-tree hash through
   clean bounded extraction. Self-calculated hashes, two downloads/mirrors, issue
   comments and a project signature over unexplained bytes do not complete this
   step. No sufficient initial authentication evidence is available yet.
2. **Record the release.** Use a versioned manifest containing engine/package and
   registry versions, CfT revision separately, platform/architecture, artifact URL,
   archive SHA-256/size, installed-tree SHA-256/layout, executable path, enrollment
   evidence reference/digest, reviewer approval, monotonic manifest version and
   validity bounds. The application must authenticate this metadata before using
   its digests; cache-local replacements and environment URLs cannot select roots.
3. **Assign key ownership.** The project owner must designate release and security
   custodians, independent reviewers, recovery custodians and the public-root
   distribution channel. Proposed separation: offline/hardware-protected root
   keys with threshold approval; a scoped release key signs only reviewed
   browser manifests. Root public keys ship through a reviewed application
   release. Private keys remain outside this repository and agent workspace.
   No custodian, key, algorithm/threshold or signing infrastructure is enrolled.
4. **Update and rotate.** Every package/revision/platform change repeats initial
   authentication and reviewer evidence. Bind delegations to artifact/version
   scope; publish root rotations authorized by the existing trusted threshold
   and new-key possession, or distribute replacement roots through an explicitly
   reviewed application update. Do not trust a new key just because the same
   download endpoint advertises it. Custodians must settle rotation/recovery
   procedures before production use.
5. **Revoke and prevent rollback.** Signed, versioned revocations must identify
   revoked keys, manifests and artifact digests. Verify monotonic versions and
   expiry; recheck cached artifacts against applicable revocations before launch.
   Unknown trust/freshness must fail clearly. Owners must define offline validity,
   trusted-clock/persistent-state handling and emergency recovery; these policy
   details have not been selected or implemented here.
6. **Keep platform gaps visible.** Publish only entries with authenticated evidence.
   Missing/expired/revoked entries must report a specific readiness error without
   downloading or launching unverified bytes. Automatic managed downloads remain
   the intended behavior for valid enrolled entries; this is not installed-only
   operation. Production adoption requires the full relevant fixture/runtime
   matrix and reviewer acceptance, independently of this research.

The concrete prerequisite is an accountable upstream authentication route plus
named project custodians and reviewer-approved enrollment/root lifecycle. Signing
a manifest alone cannot manufacture the missing upstream authenticity. The
current unsigned preparation primitives do not implement this signing lifecycle.

## Reproducible offline checks

`test/fixtures/engine-distribution.json` is a distribution-only observation, not a
trusted identity manifest. `test/engine-distribution.test.mjs` compares its exact
version and five URL mappings with the installed, lockfile-pinned registry in
read-only child processes. It also checks that this fixture cannot pass identity
enrollment. No test downloads or launches a browser. Native signature verification,
archive equality, signed-manifest verification and runtime containment remain
unverified.
