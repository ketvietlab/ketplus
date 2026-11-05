# ADR 0023: reusable browser screenshot files

Status: accepted for 0.1.31.

## Problem

The browser worker already writes real PNG captures, but the MCP facade hid their
paths. Host guidance then claimed that tools could not save screenshots and told
agents to ask the user. This prevented ordinary reuse of the exact captured image
as a project asset even when the user had authorized that work.

## Decision

The local MCP facade exposes original artifact paths. `browser_capture` returns
`path`; `browser_observe` with `includeScreenshot: true` returns `capturePath`;
`visual_compare` returns `raster.diffPath` for a generated diff. Image blocks and
capture IDs still appear in the conversation. No new browser method, arbitrary
file destination or browser permission is added. The paths identify files on the
machine running the worker, in the same filesystem used by the local agent.

Agents may use/copy the original PNG for authorized assets/evidence with their
normal filesystem tools and permission mode. They must copy it outside the
session directory before closing: temporary captures are still deleted on close.
The copied asset then belongs to the project and survives browser cleanup. They
must not reconstruct images from base64/data URIs in tool messages or print image
bytes into text. Older workers without a path require a runtime update, not a
fabricated path. Shared host instructions, MCP instructions and the bundled
browser skill describe the same contract.

## Verification

The MCP regression checks both screenshot-producing calls against the actual
PNG bytes/hash, copies the original file, closes the session, confirms temporary
files disappear and verifies the retained asset stays byte-identical. Provider
routing coverage checks that available tools receive the new guidance and that
disabled/unavailable tool sessions do not receive claims about screenshot files.
This change does not install or restart the user's running app/daemon.

Existing installations need the updated browser runtime and host policy. A
Desktop-only replacement that keeps an older daemon does not update the daemon’s
provider instructions/runtime; schedule that daemon upgrade separately while
idle. Browser sessions and active turns must not be interrupted to install.
