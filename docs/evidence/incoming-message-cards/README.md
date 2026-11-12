# Incoming message card comparison

Synthetic transcript fixture only; no account or conversation data. Captured offscreen
at 1000 × 720 in light and dark modes with `agentCardEvidence`.

The before captures use the baseline transcript QML with the same synthetic fixture.
The after captures show the four-line selectable preview and explicit Expand control.
Expansion, Collapse, reading-position preservation, incoming agent/system/team origins,
and the unchanged owner bubble are verified by the focused transcript tests.

| Theme | Before | After |
| --- | --- | --- |
| Light | ![Before, light](before-light.png) | ![After, light](after-light.png) |
| Dark | ![Before, dark](before-dark.png) | ![After, dark](after-dark.png) |

Round-two evidence also includes paired incoming/outgoing cards and expanded incoming
bodies in both themes. Task links remain available without expanding a team message.
The owner-bubble regression passes through both live normalizer user-message types;
the card selection test exercises the platform Copy shortcut and clipboard output.

| Theme | Incoming and outgoing, collapsed | Incoming expanded |
| --- | --- | --- |
| Light | ![Both directions, light](paired-light.png) | ![Expanded, light](expanded-light.png) |
| Dark | ![Both directions, dark](paired-dark.png) | ![Expanded, dark](expanded-dark.png) |
