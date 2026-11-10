# ADR 0109: Show the account that signed this Mac in

Status: accepted (2026-11-10, #166).

## Context

The mock's Settings → Account & devices page and the Connections account strip show
who signed this Mac in. The app had no such data:
- the strip showed a fixed placeholder name and address;
- Settings had only a link to Connections.

Two sign-in paths enroll a Mac: the browser sign-in and the device code. Both
redeem a short user session. The relay's `UserSession.subject` carries `id`,
`username` and `displayName` (ketplus-control v2). The daemon used that session once
to enroll and then dropped it, subject included.

The mock also shows "KetPlus Relay this month 3.1 GB of 20 GB". The relay reports
no usage, and no allowance is defined anywhere.

## Decision

- **Enrollment.** When it enrolls, the daemon stores the subject's `displayName`
  and `username`:
  - each is capped at 128 characters;
  - they go in the control credential store, next to the enrollment;
  - they are written in the same sequence as the machine id;
  - sign-out removes them with the enrollment.

  The subject id, the tokens and the user session itself are still not stored.
- **`remote.status`.** While enrolled, it reports `account {display_name, username}`.
  - It omits `account` when neither name is known, for example for an enrollment made
    before this change. That enrollment gets the names at its next sign-in.
  - Nothing goes to remote clients. `remote.status` is local only, as before.
- **Settings → Account & devices** is a page:
  - an avatar with initials, the display name, and "username · KétPlus Pro";
  - Manage connections opens Connections;
  - Sign out opens Connections, which asks first, as its own button does;
  - when signed out, the page says "Not signed in" and offers Sign in….
  It shows nothing about the account until the daemon answers.
- **Connections.** The account strip shows the same data. Only the demo mode keeps
  its synthetic account.
- **The relay usage meter is left out.** Jay chose this on 2026-11-10. Adding it
  would need a monthly allowance per plan and usage counting in the gateway.

## Consequences

- People who signed in before this build see "Signed in" until they sign in again.
- The page differs from the mock only by the missing meter.
