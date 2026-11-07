# Conversation plan harness — concept mock

The plan belongs to a conversation, not to a project-wide Planning screen. Open the **Plan** tab inside Agent Workspace. Conversation 4821 and 4822 have independent in-memory plans; switching conversations preserves progress, evidence, timers and follow-up budgets separately. No global Planning navigation item exists.

## Compact presentation

Each task is one row: completion icon, task title, owner and status. Expand a task to inspect its acceptance criterion and evidence. Activity entries are also one row; expand one for details. The conversation header and sidebar reflect the run state. The Plan tab shows the verified count.

All text is English. The footer’s Preview selector and Play continuation button are prototype controls, not proposed production controls. Concept labels and sample-evidence disclosures must remain when these captures are used in marketing.

## Demonstration

- Choose Ready and press Run plan. The first task completes, the agent ends its turn prematurely, and the simulated daemon automatically resumes the same conversation at task 2. The remaining tasks complete with fixture evidence.
- Stop run cancels queued continuation and stops the timer. Resume plan is explicit and preserves prior progress.
- Blocked retains open tasks and names the repository owner who must restore the fixture. Retry check deliberately leaves it blocked; no external dependency is actually checked or repaired.
- Limit reached preserves the attempt counter. Only Allow one more follow-up increases the budget.
- Complete ends automatic continuation. Deployment is outside the sample plan’s scope.
- Open conversation 4822 to view a separate plan; return to 4821 to see its existing progress.

These interactions are a proposed harness. There is no production daemon scheduler, persisted plan storage, task validation, tracker synchronization or provider invocation in this change. Evidence strings, commits, tests, reviews and artifacts are fictional. The existing response-guidance report is not proof that this harness is already implemented.

## Reproduce

Build the standalone mock using ../../README.md, then from the repository root:

```sh
MOCK=build/mock/KetPlusMock.app/Contents/MacOS/KetPlusMock
"$MOCK" --screen=workspace --state=plan-ready --dark
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
- QML model suite: 11 passed, 0 failed. Covers automatic timer continuation, conversation isolation, explicit stop/resume, blocker retention and follow-up limits.
- Pro smoke: 54 screens, 0 QML problems. Lite smoke: 11 screens, 0 QML problems.
- Inspected native captures for continuing and blocked in dark mode, and complete with collapsed and expanded evidence in light mode. All four tasks fit on single rows when collapsed.
- Native click automation could not attach to the preview window (`cgWindowNotFound`). Button interactions were not verified by desktop automation; model transitions and the real QML timer were tested directly. No production daemon, package installation or external tracker was tested.

## Ownership contract for future implementation

Use conversation identity plus plan revision and task identity to scope progress and evidence. A follow-up resumes that conversation and cannot silently replace its plan. Completion must be tied to verified acceptance evidence for the applicable code revision, not merely a final-answer event. Stop must invalidate queued follow-ups. Blockers and exhausted budgets must preserve unfinished work without rescheduling an unbounded loop. Persistence, stale-event handling and revision invalidation still need production design and tests.

## Marketing wording

**The turn ended. The plan didn’t.**

Concept: a conversation plan that tracks verified tasks and asks the agent to continue unfinished work, with explicit stops, blockers and run limits.
