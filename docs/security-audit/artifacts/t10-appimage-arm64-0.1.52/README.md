# AppImage ARM64 payload reconciliation

The existing `KetPlus-Pro-0.1.52-aarch64.AppImage` has SHA-256
`9451ea73dbbc5cd25a7b11b34bb73322f00e585e733a13854da385644ff7bcac`.
The complete, offline-extracted view contains **6,919 files/symlinks and 290
discovered components**, including the actual runtime prefix and five entries
outside the product prefix. Source revision is the same recorded/resolved public
revision as the [Debian dossier](../t10-deb-arm64-0.1.52/README.md). Neither artifact
was changed, installed or executed.

[reconciliation.json](reconciliation.json) binds the artifact, inventory, candidate
SPDX, artifact binding, full local review input and this dossier by SHA-256.
[components.csv](components.csv) retains every discovered component's identity,
source/notice evidence and outstanding request IDs. Shared request IDs refer to
the Debian dossier's material table; the extra runtime requests are below.

The 936,456-byte prefix matches the immutable ARM64 runtime hash in the matching
packaging pin. Its boundary is also a valid SquashFS 4.0 superblock using zstd.
`unsquashfs` 4.7.5 was built in temporary storage from the recorded official
source-archive digest; only that extractor was run. No AppImage entry point,
runtime binary, AppRun or other package executable was invoked. No extractor
was installed into the host or product. The source's mksquashfs-only Darwin
patch was unnecessary for the unsquashfs target and was not applied.

All **6,914 shared prefix entries** match Debian in content hash, type, size and
link target. All 6,863 regular files also match in mode after correcting extraction
to use a child-only umask of zero. npm, Node, public source and native ELF evidence
from Debian therefore applies to those same bytes, with paths mapped from
`opt/ketplus-pro/` to `rootfs/usr/`; source or license conclusions are not inferred
for the AppImage-specific runtime/entry points. The host symlink modes differ for
51 links between the Darwin extraction adapters. These are host snapshots, not
proof of equal archive metadata. Ownership, timestamps, xattrs and symlink-mode
metadata remain outside this inventory's verified archive coverage. The recipe
uses `-no-xattrs`; the native final audit must verify the actual filesystem metadata.

The first extraction inherited a restrictive host umask and masked 6,863 ordinary
file modes. That snapshot was discarded from the acceptance evidence. The final
extraction used the corrected collector, preserves the AppImage's executable
mode on `runtime-prefix`, and supplies the inventory/binding digests above.
The parent temporary directory remains private; the caller's umask is unchanged.

[runtime-source-evidence.json](runtime-source-evidence.json) records the retrieved
runtime source archive at `fedfad51acc2860b2916a1eef32ba4a500ff31ef`, its digest,
prefix/notice hashes and declared build inputs. This source recipe links
`squashfuse`, `squashfuse_ll`, `zstd`, `z`, `fuse3` and `mimalloc` statically against
an Alpine 3.21.0 build environment. The exact dependency package versions and
actual binary-to-build correspondence remain unverified. The six declared
logical runtime components are separate incomplete embedded entries in the local
review input, bringing its count to 296. They are not six newly discovered files.

| Request ID | Missing material | Next owner |
| --- | --- | --- |
| RUNTIME-DEPENDENCIES | Recover the exact immutable package/build inputs for musl, libfuse3, squashfuse, zstd, zlib and mimalloc from the build that produced the pinned runtime asset. The checked-in runtime notice links upstream projects, does not supply their full license/source materials, and does not name mimalloc. Obtain the applicable mimalloc notice and resolve its exact version; do not assume this declared link input is fully audited binary composition. | Release owner; runtime vendor/licensing reviewer |
| RUNTIME-SOURCE-RELINK | Prepare the matching dependency sources, patches, static link/relink materials and compliant source/offer/access terms under publisher control. The retrieved runtime repository archive alone excludes its external Alpine package inputs. | Release owner; runtime vendor/licensing reviewer |

[gate-result.json](gate-result.json) records an actual CLI gate run against this
artifact's digest, complete payload and separately bound review. Exit **1** and
absence of a reviewed SPDX output were verified. The four remaining error groups
match Debian: unresolved identities/licenses, missing notices/source materials and
incomplete embedded/dependency evidence. Debian's Qt/Chromium/ICU, SDK terms,
source-offer, link evidence and dependency-audit gaps still apply. This is no
native SDK-free smoke, signing, per-platform licensing acceptance or authorization
to publish. T-10 and PS-05 remain open.
