# Contributing to KétPlus

Contributions to both KétPlus (Lite) and KétPlus Pro are welcome: bug reports,
documentation, tests, accessibility improvements, and focused code changes.
Both desktop editions are developed in this public repository. Mobile apps and
hosted services are proprietary and outside its scope.

## Start with an issue

Search the [existing issues](https://github.com/ketvietlab/ketplus/issues) and
[open pull requests](https://github.com/ketvietlab/ketplus/pulls) before starting.
For a bug, include the edition, version or commit, OS/architecture, minimal
reproduction, expected result, and actual result. Use synthetic files and
redacted logs. For a feature, describe the user problem and an example workflow.
Use the issue templates for these reports.

Discuss large changes, new dependencies, protocol changes, and ownership or
security boundaries in an issue before implementing them. Small fixes and docs
corrections can go straight to a pull request; link an issue when one exists.
For a suspected vulnerability, follow [SECURITY.md](SECURITY.md) instead of
posting reproduction steps in a public issue or pull request.

## Set up a development checkout

Fork this repository, clone your fork, and add this repository as `upstream`:

```sh
git clone https://github.com/YOUR-USERNAME/ketplus.git
cd ketplus
git remote add upstream https://github.com/ketvietlab/ketplus.git
git fetch upstream integration
git switch -c fix/describe-your-change upstream/integration
```

Create your branch from the current `integration`, even if GitHub initially
shows another default branch. Use a short, descriptive branch and commit subject.
Keep commits focused on the change; avoid generated files and unrelated cleanup.

Read [AGENTS.md](AGENTS.md), which records repository engineering rules for
human and automated contributions, and these documents for the area you change:

- [Project status](docs/project-status.md): implemented behavior and remaining work.
- [Repository boundaries](docs/repository-boundaries.md) and
  [architecture](docs/architecture.md): ownership and extension points.
- [Development workflow](docs/development.md): prerequisites, build presets,
  machine-local configuration, and formatting.
- [Testing](docs/testing.md): target map and minimum verification for each area.
- [Product architecture](docs/product-architecture.md) and
  [licensing](docs/licensing.md): product, security, and dependency constraints.

Follow the setup and edition-specific build commands in those documents rather
than installing or signing a release. `lite-dev` configures the standalone editor;
`dev` configures Pro. Platform availability and daemon-only presets are described
in the development and testing guides. Put machine-specific overrides in ignored
`CMakeUserPresets.json`, not in committed presets.

## Implement and verify

Keep changes in the owning module. `core/` must build without Pro directories;
Pro uses exported `KetPlusCM::*` targets and public include paths. Do not add
`KETPLUS_EDITION` checks in `core/` or relative includes into `core/src`.
Hosted services enforce paid entitlements; a client build flag is not an
access-control boundary.

Add or update focused tests for changed behavior, then run the relevant minimum
verification in [docs/testing.md](docs/testing.md#minimum-verification). A `core/`
change also requires `ctest --preset lite-dev` after configuring and building
that preset. Format changed C++ with the repository `.clang-format` as described
in the development guide. Documentation-only changes require a diff/content check
and validation of edited YAML/JSON, not a native application build.

The default tests are deterministic and do not use model-provider quota. Do not
run live-provider tests unless explicitly requested by the user. Use fake
providers and synthetic fixtures; never use another person's account or data.
Record the exact commands, results, platform, and any checks you could not run.
Do not describe a local test pass as release or device acceptance.

Update relevant docs when changing protocols, ownership, security properties,
build commands, dependencies, or release behavior. Record durable architecture
decisions under `docs/decisions/`. Before committing, run:

```sh
git diff --check
python3 tests/hygiene/test_public_tree.py
```

Review new and untracked files as well as the diff before staging them. The
public-tree guard scans tracked files; run it again after staging new files.

## Keep public contributions safe to publish

Issues, pull requests, attachments, and commits are public. Do not include
credentials, tokens, signing material, user transcripts, personal data,
machine-specific paths, generated build trees, or `.cache` contents. Use
synthetic reproductions and inspect screenshots and logs before attaching them.
Do not publish private repository links, revisions, paths, service/schema names,
endpoints, internal hostnames, or infrastructure details. Describe external
ownership generically; preserve public library API names and license notices.

The source is licensed under [MPL 2.0](LICENSE), applied at file level; files
with their own notices retain those licenses. Preserve existing notices and
check [licensing](docs/licensing.md) and
[third-party notices](THIRD_PARTY_NOTICES.md) when adding or updating dependencies.
The project names and artwork have separate
[trademark rules](TRADEMARKS.md); the source license does not license the marks.

## Open a pull request

Push your branch to your fork and open a pull request with **base `integration`**
in `ketvietlab/ketplus`. GitHub may select a different base by default; check it.
Use an English title and description so external contributors can follow review.
Explain the problem, resulting behavior, issue link, and validation. State
limitations plainly and include synthetic UI evidence when relevant. The pull
request template is a short handoff; detailed test selection stays in the
[testing guide](docs/testing.md).

Respond to review in the same pull request and rerun the checks affected by
revisions. Maintainers review and merge accepted contributions. Integration PRs
run a lightweight public-tree guard; do not rely on them for the full native
build/test matrix. Follow [the release process](docs/release-process.md) for the
current CI exceptions and promotion rules.

Promotion from `integration` to `develop`, and releases from `develop` to `main`,
are maintainer-operated steps under that process. Contributors should not open
feature pull requests directly into `develop` or `main`, publish release
artifacts, or change hosted services as part of a contribution.
