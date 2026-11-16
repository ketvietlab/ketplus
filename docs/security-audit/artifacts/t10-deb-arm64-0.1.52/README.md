# Debian ARM64 component reconciliation

This dossier assesses the existing unsigned `ketplus-pro_0.1.52_arm64.deb`, SHA-256
`0d5265bf83829e2bc808269f19d3a286830a385180e2f82eb95ab09037cdeed7`.
Its complete extracted payload is unchanged: 6,925 files/symlinks, 298 discovered
components, inventory SHA-256
`5e891f94073840116aa2e7462853f94dcd5d624a270159078938ea91966007d7`.
The recorded source revision resolves to
`5b9417abc8b9b9fcf5a20301397eed9e1644ba27`. This is an older engineering artifact,
not a package built from PR #710. No package executable was run or changed.

## Evidence and verified correspondence

- [reconciliation.json](reconciliation.json) binds the artifact, inventory, review
  input and every evidence file below by digest. The local full review input is
  retained with the temporary payload; this compact dossier does not duplicate
  thousands of installed file memberships.
- [components.csv](components.csv) has one row for **every** discovered component:
  109 npm, 153 Qt SDK, three ICU, three native product executables, one Node,
  four recipe-generated files, 24 public-source files and one mixed-resource
  group. Source comparisons, declared terms, installed notice paths and material
  request IDs are separate fields. A source match is not a license conclusion.
- [npm-payload-evidence.json](npm-payload-evidence.json) is the offline comparator's
  report for both production lockfiles from the matching public source revision.
  Registry tarballs are SHA-512 checked before every owned installed file is
  compared by length/SHA-256. This includes nested packages and the native SDK;
  all 109 archives and 6,548 owned installed files matched.
  no JavaScript, native code or install hook is run. The report's file-match result
  does not approve either SDK package's restrictive license pointer.
- [source-materials.json](source-materials.json) records ten retrieved, pinned
  static-library source archives and the actual installed notice hashes. Seven
  notices match upstream byte-for-byte. miniz differs only in CRLF/LF; libjuice
  and libdatachannel differ only in trailing whitespace. Both original hashes
  and the comparison rule are retained. Node's binary **and complete LICENSE**
  match the official `v24.21.0` linux-arm64 `.tar.gz` whose digest is pinned in
  the matching source tree. The archive is distinct from Node's `.tar.xz`.
- [public-source-file-matches.json](public-source-file-matches.json) maps 89
  installed files to identical bytes in the exact public source archive, including
  27 installed license/notice files and both copies of the seven product icons.
  This also confirms the installed product THIRD_PARTY_NOTICES.md is unchanged
  from that revision. Its placeholder for Qt notices is not the missing notices.
- [elf-needed.json](elf-needed.json) records SHA-256, SONAME and DT_NEEDED from all
  134 ELF files using offline ELF64 little-endian header parsing. It finds 124
  distinct dependency names; 75 have no file/SONAME in this payload and require
  the target system. This is not a Linux loader/runtime smoke or a complete
  static dependency inventory. System-supplied libraries are distinguished from
  shipped libraries; their absence is not itself proof of a packaging error.
- [gate-result.json](gate-result.json) is the rerun of the reviewed gate against
  this exact artifact/binding after source reconciliation. CLI exit **1** was
  verified and no reviewed SPDX was emitted. All four errors remain explicit:
  unresolved component identity/license, missing notices, missing source materials,
  and incomplete embedded/dependency evidence. `publicationAuthorized` is false.

The source archives and npm tarballs are prepared in temporary storage, with the
complete extracted payload and full review input. Their receipt hashes are safe
metadata, not published source hosting or proof that an offer is under the
publisher's control. The corresponding-source/build evidence demanded by the
release contract is still required. Do not place supplementary material into an
extracted old payload and keep claiming the original artifact digest.

## Specific material requests and next owners

The IDs below are referenced by individual rows of components.csv. These are
outstanding acceptance conditions, not waived requirements.

| Request ID | Component/material and source to obtain | Next owner |
| --- | --- | --- |
| QT-BUILD-SOURCES | Exact Linux ARM64 Qt 6.11.1 SDK package hashes, module/build configuration, patches and matching Qt/Chromium corresponding source; obtain from the original native packaging build and its SDK inputs, then prepare a publisher-controlled source archive/offer. Version metadata and library filenames alone do not establish this. | Native build/release owner |
| QT-NOTICES | Actual deployed Qt module notices, LGPL-3.0/GPL-3.0 texts and exact WebEngine/Chromium third-party credits. The old payload has no matching generated Qt notice set; use the original SDK/build's license outputs, not a generic latest license page. | Native build/release owner |
| QT-THIRDPARTY-INVENTORY | Exact Qt/WebEngine third-party inventory, Chromium GN configuration and generated notices/SBOM tied to the deployed module/plugin/QML and resource hashes. | Native build/release owner |
| ICU-SOURCE-NOTICE | Source identity/build provenance and ICU/Unicode notices for libicudata, libicui18n and libicuuc (filenames say 73.2); recover from the exact Qt SDK, then validate each library digest. No applicable ICU notice was found in the payload. | Native build/release owner |
| BUILD-LINKAGE | Original configure cache, compiler/toolchain identity, linker commands/maps and static inputs for KetPlus, ketplus_daemon and ketplusctl, tied to their actual hashes. Source FetchContent pins are declarations; source availability does not prove linkage. | Native build owner |
| STATIC-SOURCE-RECONCILIATION | Reconcile the ten prepared source archives (Scintilla, KetPlus Syntax/Lexilla, libvterm, miniz, KetQt, libdatachannel/libjuice/usrsctp/plog) with link evidence; include generated/transformed sources such as the Scintilla Qt adjustment from the matching recipe and any changes. Add the accepted source/offer material to a newly assembled artifact. | Native build/release owner |
| EMBEDDED-ASSETS | Confirm embedded KÉT Sans 1.1.0, Sans Mono 0.10.4, kit tokens/symbols and theme data against resource inputs/outputs. Source font/archive pins and installed OFL/CC/MIT notices are available, but this file scan does not enumerate compiled Qt resources. | Native build owner; licensing reviewer |
| NODE-EMBEDDED-INVENTORY | Reconcile Node's embedded dependencies and notices using the complete byte-matched Node LICENSE and the official build/source inputs for the pinned binary. | Licensing reviewer |
| SDK-TERMS | Both @anthropic-ai/claude-agent-sdk 0.3.322 and its linux-arm64 native package ship restrictive `SEE LICENSE IN ...` metadata and a notice pointing to upstream legal agreements. Obtain an applicable redistribution determination/terms evidence or a separately approved package change; MIT cannot be assigned by inference from surrounding npm packages. | Release owner; licensing reviewer |
| SDK-EMBEDDED-INVENTORY | Native Claude CLI 2.1.322's embedded runtime/dependency and notice inventory, supplied by its vendor or a validated vendor distribution manifest. Byte identity with the lock-pinned npm archive does not enumerate the native binary's contents. | Release owner; vendor/licensing reviewer |
| STANDARDWEBHOOKS-NOTICE | The old installed package and its published 1.1.1 npm tarball both omit LICENSE. The version metadata's gitHead b4d2c14fc5b4ccff3ff271e3b087dff812254c59 has the MIT library notice at libraries/LICENSE, SHA-256 5ec8c7b26b64d881a6706617bed25c049f97f2f35de034c756de8546fd6dbe27. The pinned recipe supplement fixes future packages; rebuild and inspect the actual new artifact. | Developer; native build owner |
| RESOURCE-OWNERSHIP | Split/reconcile the mixed-resource group's actual product, browser/sidecar, Qt/Chromium resources, translations and notices against source/build inputs. The 89 exact public-file matches cover only a subset; do not assign one license/source to this entire group. | Native build owner; licensing reviewer |
| SOURCE-MATERIALS | A working, publisher-controlled corresponding-source archive/offer with exact dependency identities where required; prepare installation/relink instructions and place the accepted source/offer evidence in a new package. External receipt files do not satisfy the gate's installed source-material field. | Release owner |
| DEPENDENCY-AUDIT | Validate all ELF dependencies on a clean SDK-free native target, including system-package resolution and all static/native transitive components, then supply signed-off build/dependency evidence. The offline DT_NEEDED list is an input to this check. | Native build/security reviewer |
| LICENSE-REVIEW | Conclude applicable licenses/attributions for each supplied source/notice mapping. Manifest SPDX strings and equality with published bytes are technical evidence, not final legal review. Resolve all remaining NOASSERTION and license pointers explicitly. | Licensing reviewer |
| TRADEMARK-POLICY | Confirm the public TRADEMARKS.md applies to the unchanged product icon copies; the mark is not automatically licensed by the surrounding code's MPL. | Release owner; licensing reviewer |

## Scope remaining

The [separate AppImage dossier](../t10-appimage-arm64-0.1.52/README.md) now inventories
its complete payload and verified runtime prefix. Its additional runtime source,
dependency/notice and relink gaps remain explicit; a Debian result cannot cover
them. File modes here describe the Darwin extraction snapshot; archive ownership,
timestamps, xattrs and symlink modes need separate native metadata verification.
macOS and
Windows distributable artifacts also need independent evidence; local Lite
installation is implementation verification. Native rebuild/smokes, source-offer
availability and final licensing review were not performed here. T-10 criterion 1
and final artifact acceptance remain open; PS-05 is not closed.
