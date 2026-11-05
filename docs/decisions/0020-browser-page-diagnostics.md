# 0020: Read-only page diagnostics in the browser API

Status: accepted
Date: 2026-10-07

## Context

Agents debugging KetJS island pages through `browser.v0` (ADR 0008) could
observe and act but could not see why a page misbehaved. There was no way to
go back after a `pushState` facet, no console or uncaught errors, no record of
the page's requests, and no way to read `data-*` attributes on `<html>` or the
scroll position of a grid. A stylesheet served from the HTTP cache could not be
refetched. Agents fell back to keyboard shortcuts or asked for arbitrary script,
which the API deliberately does not offer.

## Decision

- `browser.act` gains `history` (`back`/`forward`, Playwright `goBack`/
  `goForward`, which traverse `pushState` entries without reloading) and
  `reload` (`ignoreCache` uses CDP `Page.reload`, as Playwright has no cache
  control).
- Each page owns a `PageDiagnostics` collector (`src/page-diagnostics.mjs`) from
  the moment it is adopted. It keeps a ring of 500 log entries and 1000 finished
  requests, with monotonic cursors.
  - Console messages come from Playwright. Uncaught exceptions and unhandled
    rejections come from CDP `Runtime.exceptionThrown` on a private CDP session,
    because Playwright reports both as `pageerror` and cannot tell them apart.
    Without CDP the collector falls back to `pageerror`.
  - Failed requests and responses with status ≥ 400 are also log entries.
  - Consecutive identical entries collapse into one with a `count`.
- Act results report `navigation`: `full-load` when CDP `Page.frameNavigated`
  commits a main-frame document, `same-document` on `Page.navigatedWithinDocument`,
  otherwise `none`. Playwright's `framenavigated` fires for both and cannot
  separate them. A `Page.getFrameTree` round trip after the action flushes earlier
  CDP events before the result is built.
- Observe and act results carry log counts, including errors since the latest
  observation, so an agent notices new errors without reading the log.
- `browser.logs`, `browser.network` and `browser.inspect` are read-only and do
  not invalidate the snapshot. `browser.inspect` runs one fixed reader in the
  page for named fields (attributes, scroll, value, bounds, text,
  `computedStyle:<property>`); it never evaluates caller-supplied script.
- `browser.observe` gains `visibleOnly`, which keeps only nodes intersecting the
  viewport and every scroll container's visible area. `<details>` and
  `<summary>` are now observed, so a disclosure menu always has a node ref.

## Security

- URLs in logs, network entries and inspected values pass through the existing
  redaction. Network entries never include headers or bodies.
- Password, one-time-code and payment-card inputs return `[redacted]` and omit
  their `value` attribute.
- All new methods require an allowed page; they read only the page the session
  already reaches under its origin policy.

## Consequences

- Buffers are bounded per page and released when the page closes.
- The private CDP session ties the collector to Chromium, which is the only
  engine the worker supports (ADR 0008).
