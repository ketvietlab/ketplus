# Product and system architecture

## Product position

Ketsuite is the parent brand, identity plane, billing plane, and distribution
site. It is not synonymous with the ERP product. KetPlus, Flow, KétRouter, and
ERP are independent products that share platform capabilities.

| Product | Position | Source model | Revenue role |
| --- | --- | --- | --- |
| KétPlus (Lite) | Lightweight native editor with workspace, Git, terminal, and document preview; default editor on Lampion | Public `ketplus`, `KETPLUS_EDITION=lite`, MPL 2.0 | Free open-source distribution |
| KétPlus Pro | Full productivity IDE with Agent Workspace, Atlas, daemon, and remote sessions | Public `ketplus`, `KETPLUS_EDITION=pro`, MPL 2.0 | Free desktop client that drives paid services |
| KétPlus App | iOS/Android remote session client | Private the private mobile app, proprietary | App Store and Play Store subscription or purchase |
| KétPlus Daemon | Local and headless Linux session/process control plane | Public `ketplus/apps/daemon`, MPL 2.0 | Host side of managed remote sessions |
| KétRouter | Multi-provider model gateway similar to OpenRouter | Private service | Usage-based credits and billing |
| Flow | Organization task and work management | Separate product outside ERP | Organization subscription |

KetPlus is distributed from `ketsuite.com/products/ketplus`. The unavailable
`ketplus.com` domain does not require changing the product name.

## Repository boundary

```text
ketplus/                         public, MPL-2.0 (ADR 0029)
  core/                          editor, workspace, Git, terminal, preview; Lite app
  apps/{desktop,daemon,browser}/ Pro shell, daemon and ketplusctl, browser runtime
  modules/{ai,atlas,browser,remote}/
  packaging/  docs/  tests/

Mobile clients and hosted services live in private repositories.
```

Until ADR 0029 the Pro desktop was the private `ketplus-desktop` repository,
which pinned the public editor as a Git submodule. That repository is archived;
its history is not published.

Pro code extends `core/` through its exported `KetPlusCM::*` CMake targets and
separate modules, not through edition compile flags inside `core/`, so the Lite
build stays the editor alone.

The branch policy is `develop` for integration and `main` for production.

## Identity and organizations

This phase authenticates native clients with the existing hosted username and
password flow and exchanges them for short-lived KetPlus credentials. An external identity provider
is not a dependency of this phase. First use creates or attaches a personal
workspace; future organization membership adds roles to the same account
subject rather than creating a parallel KetPlus identity.

Remote sessions record `owner_subject_id`, `workspace_id`, and an optional
`organization_id`. Product entitlements can belong to a subject or an
organization. Authorization is evaluated from membership and capability, not
from an assumption that a user belongs to exactly one tenant.

## Packaging and monetization

Both desktop editions are free and open source. Revenue comes from the mobile
app, the hosted relay for remote sessions, and KétRouter usage. Paid features
are enforced by those services through account entitlements, never by a build
flag: anyone may build KétPlus Pro from source, and the relay trusts nothing a
client asserts about its build.
