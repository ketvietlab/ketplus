# Conversation plan harness — concept mock

The plan belongs to a conversation, not to a project-wide Planning screen. Open **Plan** as a document in the conversation’s existing right content pane. The pane uses the editor’s KetTabBar and KetTabButton: one closable tab per file, diff, Atlas, terminal session or plan, rather than fixed content-type segments. The transcript and composer stay visible on the left; there is no Conversation / Plan switch above the transcript. Conversation 4821 and 4822 have independent in-memory plans; switching conversations preserves progress, evidence, timers and follow-up budgets separately. No global Planning navigation item exists.

## Compact presentation

Each task is one row: completion icon, task title, owner and status. Expand a task to inspect its acceptance criterion and evidence. Activity entries inside the Plan document are also one row; expand one for details. The conversation header and sidebar reflect the run state. A single checklist icon with a progress ring in the conversation toolbar opens the Plan document. Its accessible name and tooltip include the verified count and state. The ring follows live progress and uses success colour at completion. Closing the pane or changing its content does not stop or reset the plan.

The transcript has no inline Plan button or duplicated Agent/Daemon activity feed. Its compact end-of-turn report states the plan status and verified count, lists one row per task with evidence or pending work, and names the next action and owner. This report is a snapshot of the finished turn; it does not silently change as the next turn progresses. A later finished turn produces a new report. Before a plan exists, there is no completion report.

All text is English. The footer’s Preview selector and playback buttons are prototype controls, not proposed production controls. Concept labels and sample-evidence disclosures must remain when these captures are used in marketing.

## Editor-style content tabs

- Each open file has its own tab; the same path reselects its existing tab. A file and its diff are separate documents.
- Each New terminal action creates a numbered session. Draft input and fixture output stay with that session when switching tabs. No shell command is executed.
- Plan is one document per conversation. Closing its tab does not cancel the plan; the toolbar progress icon reopens it with existing progress.
- The tab strip scrolls horizontally and reveals the selected tab. The dropdown lists all open documents; `+` opens sample files, a new terminal, the plan, changes or an Atlas.
- Closing an active tab selects its neighbour; closing an inactive tab preserves the selection. The empty pane offers New terminal. Hiding the pane preserves open documents.
- The fixture file viewer is read-only; these mocks do not read or write actual repository files. Terminal sessions and tabs are in-memory and are not persisted across launches.

Preview `workspace/content-tabs` for two files, two terminals and Plan; `workspace/content-terminal` selects Terminal 2 with its independent draft; `workspace/content-file` selects NavigationRail.qml. Add `--open=content-tabs` to show the open-documents dropdown.

## Demonstration

- Choose Before prompt and press Send sample prompt. The fixture request needs several steps, so the simulated agent first reads repository memory and open issues, selects relevant issue #241, then creates Plan v1 for this conversation and starts work automatically. No separate Run plan action is required.
- Expand Repo context to inspect the sample sources. Expand a task to see its source and acceptance criterion. Unrelated issues are excluded; repo context informs the user’s request rather than adding the entire backlog to the scope.
- The first task completes, the agent ends its turn prematurely, and the simulated daemon automatically resumes the same conversation at task 2. The remaining tasks complete with fixture evidence.
- Stop run cancels queued continuation and stops the timer. Resume plan is explicit and preserves prior progress.
- Blocked retains open tasks and names the repository owner who must restore the fixture. Retry check deliberately leaves it blocked; no external dependency is actually checked or repaired.
- Limit reached preserves the attempt counter. Only Allow one more follow-up increases the budget.
- Complete ends automatic continuation. Deployment is outside the sample plan’s scope.
- Open conversation 4822 to view a separate plan; return to 4821 to see its existing progress.

These interactions are a proposed harness. There is no production daemon scheduler, persisted plan storage, task validation, tracker synchronization or provider invocation in this change. Repository files, issue #241, evidence strings, commits, tests, reviews and artifacts are fictional. The demo has a deterministic multi-step fixture, not an implemented prompt classifier or a live GitHub connection. A production agent should handle simple requests directly when no plan is useful; this branch does not mock the direct-answer path. The existing response-guidance report is not proof that this harness is already implemented.

## Reproduce

Build the standalone mock using ../../README.md, then from the repository root:

```sh
MOCK=build/mock/KetPlusMock.app/Contents/MacOS/KetPlusMock
"$MOCK" --screen=workspace --state=plan-ready --dark
"$MOCK" --screen=workspace --state=content-tabs --dark
"$MOCK" --screen=workspace --state=content-terminal --light
"$MOCK" --screen=workspace --state=content-file --light
"$MOCK" --screen=workspace --state=content-tabs --open=content-tabs --dark
"$MOCK" --screen=workspace --state=plan-planning --dark --screenshot=mockups/qt/marketing/plan-harness/planning-dark.png
"$MOCK" --screen=workspace --state=plan-continuing --dark --screenshot=mockups/qt/marketing/plan-harness/continuing-dark.png
"$MOCK" --screen=workspace --state=plan-complete --light --screenshot=mockups/qt/marketing/plan-harness/complete-light.png
"$MOCK" --screen=workspace --state=plan-blocked --dark --screenshot=mockups/qt/marketing/plan-harness/blocked-dark.png
"$MOCK" --screen=workspace --state=plan-complete --open=plan-evidence --light --screenshot=mockups/qt/marketing/plan-harness/evidence-light.png
QT_QPA_PLATFORM=offscreen qmltestrunner -input mockups/qt/tests
QT_QPA_PLATFORM=offscreen "$MOCK" --screen=workspace --state=plan-continuing --smoke
```

Other direct states: `plan-running`, `plan-paused`, `plan-limit`. Direct state previews do not autoplay. Native captures are 1440×900 logical pixels, 2880×1800 on the tested Retina display.

## Verification

- Standalone mock build passed.
- QML model suite: 23 passed, 0 failed. Covers automatic continuation, stop/resume, blockers and limits, plus document identity, tab closure/selection, independent terminal drafts and output, conversation isolation, and report snapshots that retain a finished turn’s progress while the next turn runs.
- Pro smoke: 58 screens, 0 QML problems. Lite smoke: 11 screens, 0 QML problems.
- Native captures cover planning, continuing and blocked in dark mode, and complete with collapsed and expanded evidence in light mode. The plan shares the existing half-width right pane; conversation content and the composer remain visible. All four tasks fit on single rows when collapsed.
- Inspected the multi-document strip and open-tabs menu in dark mode, plus Terminal 2 and the file viewer in light mode. Fixed initial overflow positioning so the selected tab is visible.
- Toolbar/report revision: refreshed all captures. Verified the partial progress ring and compact report in dark mode, the full success ring and completed report in light mode, and the empty planning ring and blocked report states. Every report task remains a single row at the captured size.
- Native click automation could not attach to the preview window (`cgWindowNotFound`). Button interactions were not verified by desktop automation; model transitions and the real QML timer were tested directly. No production daemon, package installation or external tracker was tested.

## Ownership contract for future implementation

Use conversation identity plus plan revision and task identity to scope progress and evidence. A follow-up resumes that conversation and cannot silently replace its plan. Completion must be tied to verified acceptance evidence for the applicable code revision, not merely a final-answer event. Stop must invalidate queued follow-ups. Blockers and exhausted budgets must preserve unfinished work without rescheduling an unbounded loop. Persistence, stale-event handling and revision invalidation still need production design and tests.

## Marketing wording

**The turn ended. The plan didn’t.**

Concept: an agent that recognizes multi-step work, creates a conversation plan from relevant repository context, tracks verified tasks and asks the agent to continue unfinished work, with explicit stops, blockers and run limits.
