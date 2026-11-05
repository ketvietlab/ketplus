# R00-D CI Evidence Receipt

Recorded 2026-09-12 after hosted completion. Reviewer: Integration/Codex.
This receipt is a later documentation update, not a relabeling of historical
baseline, owner measurements or tested checkout as the latest PR HEAD.

## Exact Source Stack

| Input | Immutable revision |
| --- | --- |
| #18 validator/tests source tested | `335257a0632b91f26d57a21fb770abe2e0026b39` |
| #21 Daemon characterization dependency | `79f802a1062936d469068f18bd121a0d9479c215` |
| #22 workflow source tested | `cf671e2019686d6019592a3dacf484a6a83c88ef` |
| Plan checker historical source | `37a14562a11b6ec9fb7f137e9e93c95af464bc8d` |
| Core #19 merged dependency | `0d789192596d01a387d9dc69efae1ec53de13b25` |
| Successful stack checkout/build | `d0c824a7daac48dfce94b0b378ad0a5a10f29a7f` |

Branch: `integration-r00d-ci-stack`. Only this test branch combines the owner
commits; no PR was merged into develop. #22 remains workflow-only. Its standalone
[run 34686085036](https://github.com/ketvietlab/ketplus-desktop/actions/runs/34686085036)
fails at explicit dependency ancestry checks, because #18/#21 are absent from
that checkout. It is not flaky and was not skipped. No diagnostics existed for
that pre-build failure; artifact upload also failed rather than silently pass.

The temporary ancestry checks require these exact commits. If dependencies are
squash-merged later, Integration must explicitly update the checked dependency
revisions after source equivalence review and rerun the complete stack. A squash
merge is not silently accepted through an exists-check or test omission.

## Hosted Results

[Design evidence run 34686115400](https://github.com/ketvietlab/ketplus-desktop/actions/runs/34686115400):
**PASS** for Node checks and both isolated Release characterization lanes.
Named steps ran fixture generation, configure/build, metrics QtTest, four Node
negative/provenance checks, six-entry characterization and final artifact gate.
Node lane includes 104 evidence-validator tests, integration checker, plan
checker and Core generator/harness tests. No test skip used to remove a missing
dependency. Test fixtures use synthetic data; no provider or user journal.

[Full Qt regression run 34686115397](https://github.com/ketvietlab/ketplus-desktop/actions/runs/34686115397):
**PASS**, macOS and Ubuntu configure/build/full CTest on the same stack. Existing
build.yml was not modified. This is separate evidence, not a substitute for the
isolated characterization lane.

Both downloaded metric reports were independently revalidated against the clean
stack checkout, recomputed source digest, dependency receipt files and nonempty
Qt/Node/characterization logs. Both have `status=complete`, `baselineValid=true`,
`testExitCode=0`, `sourceDirty=false`, `buildType=Release`, checkout/build SHA
equal to the stack above, four completed slots and nine distinct samples:
indices 0, 1, 2 for each of items-1k, items-10k and giant-turn.

Shared production/test source digest:
`d3951e9a0956991bbc40d9aa0297b000ab8f659de6a8b24555d70ee90b1badf1`.

| Artifact | Environment / publication UTC | Metrics JSON SHA-256 |
| --- | --- | --- |
| [macOS complete baseline](https://github.com/ketvietlab/ketplus-desktop/actions/runs/34686115400/artifacts/10296076068) | macOS 26.6.2 arm64, Qt 6.11.1, AppleClang 21.0.0.21000101; `2026-09-12T09:33:33.205Z` | `8b752fc94631e068d0387cd526ab51a4686dbfb1db3e5f269752337619d1d80e` |
| [Linux complete baseline](https://github.com/ketvietlab/ketplus-desktop/actions/runs/34686115400/artifacts/10295013988) | Ubuntu 24.04.5 x86_64, Qt 6.11.1, GNU 13.3.0; `2026-09-12T09:33:54.214Z` | `aab34e8b01061eeb188be149303e4d77063ca2576ecc872025027172afbeb7b8` |

GitHub archive digests (distinct from metrics JSON hashes): macOS
`7dd7c530875c5661c199936f5c01203e561bb7170937307dcda73d81cb38043a`;
Linux `cbfca0555760772264793927e5274d735be714f12606370223f6e88f5fb0f300`.

Revalidate after downloading an artifact in the exact clean stack checkout:

```sh
node tasks/desktop-refactor/wave-1/integration/check-characterization.cjs "$ARTIFACT_DIR/r03-metrics.json"
```

## Preserved Failures And Local Evidence

- Initial stack `860cb3625e0c29cf156b1fa02aa2451390e693ce`, workflow
  `9c9f9758b9be48e6f68bb1c467238d0a0a791fef`,
  [run 34685996011](https://github.com/ketvietlab/ketplus-desktop/actions/runs/34685996011):
  both characterization lanes **FAIL** at clean-source validation. Observed:
  gha-setup-ninja installed ninja-build inside checkout, and validation reported
  `checkout is dirty`. Fix in #22 moves only the tool destination to runner.temp;
  no baseline-validity rule or runtime test was relaxed.
  [macOS diagnostic artifact](https://github.com/ketvietlab/ketplus-desktop/actions/runs/34685996011/artifacts/10295766525)
  is explicitly not a successful baseline.
- Independent local run on that initial stack: macOS 26.2 arm64, Qt 6.11.1,
  AppleClang 21.0.0.21000101. Configure/build PASS; MetricsArtifactTest 8/8;
  Node negative/provenance 4/4; CharacterizationTest 6/6. Local metrics gate
  PASS with clean matching build SHA, same source digest and nine samples.
  Local metrics SHA-256:
  `3a232efb01e19560c94855e1c1c8134ecb658d0e7f625549bbcf204f9b9200ac`.
  This local result does not replace either hosted artifact.
- Historical [run 34675651929](https://github.com/ketvietlab/ketplus-desktop/actions/runs/34675651929)
  remains unresolved for AgentWorkspaceViewTest sequential question handling.
  New green Qt runs neither explain that failure nor justify calling it flaky.

## Gates Still Blocked

Characterization confirms existing v1 behavior, including giant-item rejection
and within-turn truncation. It does not prove lossless replay, native frame/RSS,
encryption/upload, API freeze, visual correctness or designer consent.

Backend R02-B/frozen canonical identity, continuation, encoded byte budgets,
catalog/lifecycle and TTL/relay contracts remain required. CM API/native metrics
approval, Core/consumer implementation scope and real designer/user reference
approval remain separate inputs. Mobile M04-A and M01-A retain distinct owners.
See [capability readiness and source review](r00-d-resolution.md). No wave-wide
gate was opened by these artifacts or by the green stack.
