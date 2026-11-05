# 0001: Public and private source boundary

Status: superseded by [0029](0029-one-public-repository-two-editions.md)
Date: 2026-09-10

## Context

KetPlus has a public editor distribution and a proprietary full desktop product.
The full product needs the editor implementation without duplicating source or
allowing private behavior to leak into the public repository. MPL 2.0 obligations
must remain explicit for modified public files.

## Decision

KetPlus CM lives in the public `ketplus` repository under MPL 2.0. The private
`ketplus-desktop` repository consumes an exact production-tagged CM commit as the
`vendor/ketplus-cm` Git submodule and links exported `KetPlusCM::*` CMake targets.
Private implementations remain in separate private files and modules.

Public `develop` is integration and public `main` is production. A private CM
upgrade follows a tested `cm-v*` tag from public `main`; builds never fetch a
moving branch implicitly.

## Consequences

- Public changes are released before the private gitlink advances.
- A private commit is reproducible from its submodule SHA.
- Modifications to distributed MPL-covered files are published in public CM.
- Cross-boundary interfaces must be exported deliberately through CMake targets.
- Coordinated changes may require commits and CI in both repositories.

## Alternatives considered

- Copying CM source into private was rejected because it creates drift and an
  ambiguous licensing boundary.
- Following public `main` or `develop` at build time was rejected because builds
  would not be reproducible.
- Treating the public application as a prebuilt binary was rejected because the
  private desktop must compose and link editor capabilities directly.
