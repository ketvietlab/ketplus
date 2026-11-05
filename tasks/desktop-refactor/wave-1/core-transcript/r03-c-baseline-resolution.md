# R03-C Baseline Review Resolution

Reviewed parent: `c1dc1cbbedd9dea82f603f726794dfb2c8261cb2`.
Develop verified through fetch and ls-remote: `73bddc13aab3425636fbd2f0a61860d95d13402c`.

| P2 | Resolution and evidence |
| --- | --- |
| Exit zero without artifact | Harness uses a fresh temporary manifest path and compares parsed content with the complete deterministic version/seed/scenario/integrity specification. `subprocessStatus` and `artifactValidation` are separate; validation failure skips benchmark and exits nonzero. |
| Stale artifact | Pre-existing destination is never used as input; only newly generated temporary output can be published. Tests preload a valid destination then run an exit-zero/no-output stub. |
| Invalid artifact | Stub tests cover missing, malformed JSON, wrong seed/version/integrity and report/manifest collision. |
| Replay cursor namespace | Fixture v3 cursors start at 1 for each session/generation. There is no global event cursor. |
| Replay ordering | Sort by `(offset_ms, operation_ordinal)`. Ordinal is a stable tie-breaker. Giant stream now has explicit deliveries. Live/page references are checked during generation. |
| Gap semantics | Contiguity belongs only to the explicitly contiguous negative raw stream; compact/cloud projections may omit events. |
| Media | Reference parsing only. No synthetic image resolver or decode/render evidence is claimed. |
| CI | Local Node tests pass; hosted CI registration remains pending Integration ownership. |

Integration patch request (serial, workflow owner applies): add Node setup (Node 22 or later) and the following step to the existing deterministic private CI jobs, with ordinary failure propagation and no continue-on-error:

```yaml
- name: Transcript workload and harness checks
  run: node --test tests/fixtures/transcript-refactor/generate-transcript-workloads.test.mjs tests/fixtures/transcript-refactor/baseline-harness.test.mjs
```

Run the same command locally. The generator suite materializes every published scenario and checks hashes, counts and bytes; the harness suite uses stub subprocesses. No native build/provider is required.

The harness's reference-manifest calculation is validation work, not daemon performance. Native viewport/frame/layout/WebView/RSS remain `not_measured`. Production C++ characterization belongs in a separate stacked PR; Backend still owns vNext contract freeze.
