# Manual compact window · Desktop mock

This Atlas bundle is the interactive prototype used before implementation, with deterministic data and no provider calls. Native implementation and its separate verification are recorded below. Runtime threshold resolution remains provider-owned and unverified.

## Interaction and scope

- Open the existing model picker. **Model & context** keeps model selection and reasoning, then adds **Auto-compact**.
- The Opus 5.5 example keeps **1M model capacity** and requests **272,000 tokens** for compaction. The example is not a new provider-wide default.
- Presets: **Auto / 272k / 500k / 1M / Custom**. Custom accepts whole token counts from 100,000 to 1,000,000; the daemon remains the validation authority and the provider can resolve a lower threshold.
- **Auto** clears the explicit override. Unknown model capacity is labeled as not reported.
- Changes belong to this conversation and apply from its next turn. Apply acknowledges a simulated change; Cancel keeps the saved window. Mobile Cancel also restores the original reasoning choice.
- Active turns, background tasks, offline/reconnecting and read-only access prevent changes. Failed/conflicting changes keep the draft and expose recovery. Providers without custom compact support expose Auto.
- No cache probing, automatic cache/TTL policy, cost estimate, custom summarizer or immediate compaction action is added.

## Review

Select **Set a compact window** in the existing Atlas viewer, then **Try this screen**. The flow includes the 272k picker, Custom, invalid input, Auto, applying, apply failure, conflict, offline, active turn, background tasks, loading, read-only access and unsupported runtime.

Direct entry: [`screens/workspace.html?provider=Claude&model=Opus%205.5&dialog=model&compactWindow=272000`](screens/workspace.html?provider=Claude&model=Opus%205.5&dialog=model&compactWindow=272000).

`compactState=invalid|error|conflict|offline|running|background|loading|saving|denied|unsupported` selects deterministic review states. `compactWindow=350000` opens a custom value; `compactWindow=auto` opens provider default. Add `theme=light|dark` for appearance.

## Verification scope

The authored screens use their existing HTML presenters and product token adapters. The design-system assets are unchanged. Browser checks verify the prototype rather than Qt, SwiftUI or Compose. VoiceOver/TalkBack, native Dynamic Type, real-device performance, packaging and installation are not certified.

The model catalog is bounded fixture data; 10,000-model virtualization, production authorization, server-side concurrency and real network retries are outside the mock. Product content remains English; RTL and translated production copy are not added. Existing long labels wrap or retain their original ellipsis treatment.

Verification results are recorded below after browser and focused test checks. Local raster evidence is kept outside the distributable bundle.

## Verified on 2026-10-08

- KetPlus Browser, Chromium 154.0.8037.95: standalone screens checked at 360, 390, 768, 1024, 1280, 1440 and 1920 CSS pixels in both themes. Panel/footer bounds, horizontal overflow and mobile 44px compact-control targets were measured.
- Invalid, error, conflict, offline, active turn, background task, loading, applying, read-only and unsupported states captured in both themes. The default, custom and recovery interactions were exercised on both platforms.
- 1x and 2x raster checks. Mobile 200% text preview at 360/390; desktop uses a half-size CSS viewport at DPR 2 for a 200% equivalent layout check. Native browser zoom and native accessibility text scaling are not certified.
- Keyboard Tab/Shift+Tab containment, arrow-key preset choice, Escape, Cancel preserving 272k, Apply saving 350k, Auto reset and Retry preserving 500k verified. New compact-control text colors meet 4.5:1 contrast in both themes.
- KetAtlas 0.5.2 validate and strict audit pass for both bundles. The new workflow is discoverable and its preview opens.
- Desktop DOM/flow suite: 274/274 passed. Mobile focused compact-window tests: 4/4 passed. Full mobile suite: 26/34 passed; the same eight failing tests were reproduced on clean baseline commit 2945bb5 (22/30 passed). These existing fixture/native-coverage failures were not changed by this mock task.

**Viewer limitation:** the installed KetAtlas 0.5.2 static server does not supply the font CORS header required by opaque sandboxed iframes. Embedded previews report font CORS failures and can use a fallback font. Standalone screenshots use the local Inter assets. Embedded font parity remains unverified until the preview server serves those fonts with the required CORS headers; sandbox permissions are unchanged.

2026-10-08 mock follow-up: the bundle now loads `assets/fonts-inline.css`, generated
from the same three local Inter WOFF2 assets. This removes those font requests from
opaque previews without changing KetAtlas or relaxing iframe sandbox permissions.
The earlier limitation above describes the original review revision.

## Native desktop implementation and verification

The Qt model picker now implements the approved controls with a conversation-scoped draft. Apply sends the opening configuration revision and waits for the daemon receipt; Cancel discards the draft. Failure retains the draft and refreshes the fence for explicit Retry. Auto sends null, older daemons receive no compact field, and Cursor exposes provider default. Custom values are whole token counts bounded by the portable range and any smaller reported model capacity. Model capacity remains separate; an absent capacity is never inferred from a model name.

The scrollable body keeps its footer visible at narrow window sizes and reveals a focused compact control. It uses the existing private desktop typography cascade and semantic tokens; native controls and typography are intentional platform differences from the HTML prototype. This change does not introduce cache probing, TTL scheduling, a summarizer or an immediate compact action.

- Full Debug build passed. The original full private CTest run passed 57/58; Mermaid's event-loop timing assertion failed in that run, then passed in isolation. The complete Agent and Mermaid targets both passed together (2/2). PR preparation rebased the change onto `develop` at `a8bf5b5`; the post-rebase full Debug build and all 59 private CTest targets passed.
- Fake-provider/SDK compact-window and usage checks passed 19/19 without authentication or model requests.
- Native Qt 6.11.1 Cocoa/Metal at DPR 2 passed the focused picker test. The 15-state matrix covers default, custom, invalid, Auto, failure, conflict, denied, offline, active/background work, applying, loading, unsupported, empty and unknown capacity at 1000×900 and 420×480 in both themes. It measures popup/footer bounds and checks Apply/Cancel/reset/Retry, captured revisions, arrow-key selection, focused-control visibility and Escape. The matrix plus regular captures produced 64 PNGs under ignored `build/context-harness-mock/`.
- Changed C++ was checked against the repository formatter; whitespace checks passed. The public CM submodule remains pinned to `cm-v0.3.3` without edits.

The existing files above 1,000 lines (`SessionHost`, `LocalJournal`, `AgentSessionManager`, `AgentWorkspaceView`, `CodexAppServer` and `AgentWorkspace.qml`) retain only their owned persistence/lifecycle or property/signal integration. New policy, configuration-apply and compact-control behavior lives in `CompactWindowPolicy`, `AgentSessionConfiguration` and `CompactWindowControl`; unrelated lifecycle and transcript refactoring is separate work.

Native captures use deterministic catalog/receipt data. They do not certify OS foreground activation, a screen-reader walkthrough, a physical device, packaging or installation. Provider-resolved thresholds, the timing of real compaction and billing/cache savings have not been tested with quota-consuming requests. The browser-only viewer font limitation above remains separate from the native implementation.
