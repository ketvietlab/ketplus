---
name: enterprise-grade
description: Hold UI design and feature work to the standard of a major software company. Use for any UI design, mockup, screen, component, visual polish or feature implementation — especially when the user asks for "chuẩn", "pixel perfect", "enterprise", or complains about half-finished work. Demands pixel-exact output verified by measurement, complete state coverage, and enterprise-grade feature thinking; forbids partial delivery presented as done.
metadata:
  short-description: Big-company standard for UI and features; no half-measures
---

# Enterprise Grade

Work as if this ships tomorrow from a company the size of Apple, Microsoft or Stripe, to millions of paying customers and enterprise buyers who audit every detail. A reviewer from their design and platform teams will look at your result. "Mostly works" and "looks about right" are failures.

This skill sets the bar. It does not replace the product's design system or framework skills (`ket-design-system`, `ket-atlas`, repository AGENTS.md). Load those as usual; this skill decides when the work is finished.

## Non-negotiable rules

1. **No half-measures.** Never deliver a stub, placeholder, `TODO`, "for now", hard-coded sample, happy-path-only flow, or a visual that only matches at one size or theme, while calling the work done. If the full result is out of reach, say exactly what is missing and why. Never hide gaps behind a confident summary.
2. **Exact, not approximate.** Every size, spacing, radius, border, color, font size, weight, line height and alignment comes from a design token or an explicit reference value. No magic numbers, no "close enough", no eyeballed offsets. A 1px misalignment, a blurry half-pixel edge, a clipped glyph or an inconsistent gap is a defect.
3. **Verify by measurement, not by impression.** A glance at a screenshot is not verification. Measure rendered geometry (computed styles, bounding boxes, zoomed screenshots, pixel diff against the reference) and compare to the spec.
4. **Every state, not just the demo state.** A screen is not done until all of its states are designed and implemented (see the state matrix below).
5. **Fix the root, not the symptom.** Correct the token, component or layout rule at its source. Never add a one-off override to make a single screen look right.
6. **Honest reporting.** Report what was verified, how, and what was not. Never claim a check you did not run.

## Before designing or building

- Read the brief, existing product screens, the design system (tokens, components, patterns) and any reference designs. Reuse real components; do not imitate them.
- Write down the user goal, the primary task, the roles who use it, and the data volume it must handle. Decide the information hierarchy before choosing pixels.
- Look at how leading products solve the same problem (Linear, Figma, GitHub, Stripe, Apple HIG, Microsoft Fluent, Atlassian). Match their level of polish and consistency, not their branding.
- Ask the user only when a decision materially changes the result and cannot be inferred. Otherwise choose the conventional enterprise default and record it.

## Visual standard (pixel-exact)

- **Grid and spacing:** a single spacing scale (e.g. 4/8 px) throughout. Aligned edges line up exactly across sections. Equal elements have equal gaps.
- **Typography:** use the type scale tokens only, with a consistent baseline rhythm. Text is never clipped, overlapping or truncated without an ellipsis and a full-value tooltip. Numbers in tables are tabular and right-aligned.
- **Geometry:** borders sit on whole device pixels at 1x and 2x. Radii are consistent per component level. Icons come from one family, at one size per context, optically centered, on the pixel grid.
- **Color:** use semantic tokens only. Text and controls meet WCAG 2.2 AA contrast in both light and dark themes. Never convey state by color alone.
- **Hierarchy:** one primary action per view. Secondary and destructive actions are visually distinct. Density is deliberate and consistent with the rest of the product.
- **Motion:** purposeful, short (roughly 100–250 ms), standard easing, and it respects `prefers-reduced-motion`. No layout jumps or flicker during loading, hover or transitions.
- **Responsiveness:** verify at every supported width (at minimum 360/390, 768, 1024, 1280, 1440, 1920 for web; the product's real window sizes for desktop). Nothing overflows, overlaps or scrolls horizontally unless intended.
- **Platform conventions:** follow the target OS conventions for menus, shortcuts, window chrome, focus rings and scrolling.

## State matrix (required for every screen and component)

Cover each of these, or state explicitly why one does not apply:

- default, hover, focus-visible, active/pressed, selected, disabled, read-only
- loading (skeleton or progress, no layout shift), empty (first use and filtered to nothing), partial data, error with recovery action, offline / reconnecting, permission denied
- long content: very long names, 3× text length (i18n), RTL if supported, 0 / 1 / 10,000+ items, huge numbers, missing optional fields
- light and dark theme, 1x and 2x density, keyboard-only use, screen reader, 200% zoom

## Feature standard (enterprise thinking)

For every feature, design and implement answers to these questions, or explicitly scope them out with the user:

- **Roles and permissions:** who can see, create, edit, delete, approve? Is the UI consistent with server-side enforcement (never UI-only security)?
- **Data integrity:** validation on client and server, idempotent operations, concurrency (two people editing the same thing), conflicts, partial failure, retries, undo or confirmation for destructive actions.
- **Scale and performance:** pagination or virtualization, search, filtering, sorting, bulk actions, performance budgets for large accounts. No N+1 calls or unbounded lists.
- **Auditability:** who did what, when; history or activity log where relevant; exportable data.
- **Reliability:** clear errors with a next step, never raw stack traces; graceful degradation; recovery after a crash or reconnect without losing user input.
- **Security and privacy:** least privilege, no secrets or personal data in logs or URLs, safe defaults, explicit consent for outward-facing actions.
- **Accessibility and i18n:** full keyboard path, labels, focus order, announcements; no hard-coded strings; locale-aware dates, numbers and currencies.
- **Configurability and admin:** sensible defaults with admin controls where an organization would expect them.
- **Observability:** meaningful logs and metrics for failure paths, without user content.
- **Migration and compatibility:** existing data, settings and older clients keep working.
- **Tests:** unit and integration tests for logic, plus regression coverage for each state the feature adds.

## Verification (mandatory before calling anything done)

1. Render the real screen in the real framework (browser tool, simulator or the app itself). Do not judge from source code alone.
2. Capture screenshots for every state in the state matrix, in both themes and at each required size.
3. Measure: read computed styles and bounding boxes for key elements. Check spacing, alignment, sizes and colors against the tokens or the reference. Zoom into edges, icons and text baselines.
4. When a reference design exists, compare side by side or with a pixel diff at the same viewport and scale. Treat every unexplained difference as a defect.
5. Exercise the interactions: keyboard-only pass, focus order, error paths, slow network, empty data, large data.
6. Run the project's tests, lint and formatter. Add tests for what you built.
7. Re-check after every fix. One fix often moves something else.

Keep iterating until all checks pass. Stop only when the result is complete, or when a real blocker needs the user's decision.

## Definition of done

Done means all of the following are true:

- every item in the state matrix is implemented and verified, or explicitly scoped out with the user;
- there are zero known visual defects at the supported sizes and themes;
- the feature checklist is answered in the implementation, not only in prose;
- tests pass and no shortcuts were left behind.

## Final report format

Keep it short and factual:

- **Delivered:** what now exists, with file links.
- **Verified:** each check, how it was measured, and the result (sizes, themes, states, tests).
- **Not verified / remaining:** the exact gaps and why. Write "none" only if that is true.
- **Decisions made:** defaults chosen without asking, so the user can override them.
