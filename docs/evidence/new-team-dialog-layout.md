# New team dialog layout verification

Date: 2026-11-16. Product and test sources: `8c619a088365dc14140ab0845b7144d034bc9f29`.
Environment: macOS 26.2, arm64, Qt 6.11.2, Debug Pro, offscreen Qt fixtures.
The follow-up evidence commit changes documentation only.

## Layout and focused checks

The real Qt form uses a fake daemon. English and Vietnamese geometry cases pass
at 1280 × 900, 480 × 640 and 1024 × 420. Representative English card sizes are
960 × 785, 432 × 572 and 960 × 352 respectively. Footer actions remain inside
the host window and outside the scrolling body; no horizontal scrolling is needed.
The narrow form stacks the columns. Wrapped helper text shrinks after width
changes, and keyboard traversal reaches settings and scrolls them into view.

`ketplus_team_dialogs_tests`, `ketplus_team_goal_form_tests` and
`ketplus_team_page_language_tests` passed. These include existing defaults,
validation, mentions, submission, resize and live zoom coverage at 100%, 125%
and 150%. Desktop English and narrow Vietnamese Qt captures were inspected;
capture reproduction is described in [Testing](../testing.md#new-team-dialog-layout).

## Required full matrix

`cmake --build --preset dev -j 6` completed successfully (exit 0).
`ctest --preset dev` then completed all 209 registered suites in 2382.17 seconds:
202 passed, six failed and one skipped (exit 8). The initial failures are retained
in this record rather than described as a clean full run.

| Initially failing suite | Initial failure | One serial retry |
| --- | --- | --- |
| `ketplus_chat_mermaid_tests` | GUI switch gap 216 ms exceeded the 153 ms limit in `reading-above-the-diagram` | Passed, 20.97 s |
| `ketplus_daemon_lifecycle_tests` | `journalsEachChangedFileByItsWorktreePath`: 3000 ms wait; QTest reported 4250 ms would suffice | Passed, 60.05 s |
| `ketplus_daemon_claude_adapter_tests` | `deferredSteerAnswersWithTheProviderOutcome`: 3000/3950 ms; handoff before desktop acknowledgement: 5000/9750 ms | Passed, 11.74 s |
| `ketplus_daemon_cursor_adapter_tests` | `approvalMapsToAllowOnce(default-id)`: 3000/3350 ms | Passed, 23.31 s |
| `ketplus_agent_messaging_tests` | `providersReceiveTheirOwnAgentServer`: 5000/5900 ms | Passed, 20.49 s |
| `ketplus_team_reply_tests` | `theOutboxGitHoldsUpNothing`: `pointed == 2` assertion | Passed, 31.06 s |

The full run overlapped other build/test work. After those processes ended,
`ctest --preset dev --rerun-failed --output-on-failure -j 1` ran only these six
suites once: six passed, zero failed, exit 0, 167.64 seconds. Sources, assertions,
timeouts and timing thresholds were unchanged between runs. The initial failures
did not reproduce in this retry; this observation does not establish their root
cause or prove flakiness. The full 209-suite matrix was not restarted.

The skipped suite was `ketplus_ssh_install_transaction_tests`: its existing
Linux-only fixture exits 77 on macOS because it requires Linux process and peer
credential facilities. Authenticated/live opt-in cases remained disabled; their
per-case skips are separate from this CTest suite-level skip. No test was disabled
to obtain the retry result. Raw full-run and retry logs, plus the original CTest
failure list, are retained locally outside the repository.

Formatting, `git diff --check` and all four public-tree hygiene checks passed.
No installed app was replaced, no real team was created and no provider call or
workflow dispatch was made. Review and merge remain separate gates.
