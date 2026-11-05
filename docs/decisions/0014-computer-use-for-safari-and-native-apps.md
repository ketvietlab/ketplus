# 0014: Computer use for Safari and native apps

Status: accepted
Date: 2026-09-25

## Context

ADR 0013 routes real-browser work to `ketplus_browser` and tells agents to
report a limitation instead of falling back to any other browser or
automation. Agents read that as a ban on computer use (provider tools that
control the desktop screen and input), including for Safari, which
`ketplus_browser` cannot drive because it is Chromium only, and for native
applications that have nothing to do with a web browser.

## Decision

The routing policy from `modules/browser/BrowserAgentInstructions`, the
`ketplus_browser` MCP initialize instructions and the built-in
`ketplus-browser` skill allow computer use in two cases:

1. **Safari.** When a task specifically needs Safari or WebKit (Safari-only
   behaviour, rendering or bugs, or the user asks for Safari), the agent may
   drive Safari with computer use and says why. This also applies when the
   KetPlus browser tools are disabled.
2. **Native desktop applications** that are not web browsers.

Everything else in ADR 0013 stands. Chrome, Chromium and other browser work
stays on `ketplus_browser`. Computer use must not drive Chrome, Edge, Firefox or
another browser in its place. A failing or missing `ketplus_browser` is still
reported rather than worked around. Origin denials must not be bypassed
through Safari or any other browser.

## Consequences

This amends ADR 0013's guidance; it is still model guidance, not an
enforcement layer. KetPlus does not add or remove provider tools. Computer
use keeps the provider's own availability and permission prompts, and a
Safari session is not covered by the browser origin proxy. The "use Safari"
judgement is left to the agent, which must state its reason, so a user can
see when the exception was taken.

## Alternatives considered

- Keep the ban and add WebKit to `ketplus_browser`. That would keep the origin
  proxy in force for Safari-engine checks, but Playwright's WebKit is not
  Safari and would not reproduce Safari-only behaviour or bugs.
- Allow computer use for any browser. That would bypass the origin policy and
  evidence rules of `ketplus_browser` for Chromium tasks it already supports.
