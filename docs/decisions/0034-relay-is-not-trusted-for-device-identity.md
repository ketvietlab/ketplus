# 0034: The relay is not trusted for device identity

Status: accepted
Date: 2026-11-06

## Context

End-to-end encryption keeps transcript content away from the relay's storage
([ADR 0024](0024-webrtc-transport-and-forward-only-relay.md)), but the review in #83 found that the
host took the relay's word for **who** each key belongs to. `distributeKeys` wrapped every session
key to every device in `GET /v2/devices`. Command signatures and WebRTC offers were checked against
keys the relay served. The pairing confirmation code was the relay's `claimDigest`, which the host
never recomputed. An active relay (a compromised server or a malicious operator) could therefore
add a device row with its own keys and read every session, send commands and open peer links.

## Decision

- **The relay may be actively malicious.** Content, device identity and host identity must stay
  safe against it. Availability and metadata (sizes, timing, ids) are out of scope.
- **Devices are pinned per Mac.** `DevicePins` records each trusted device's id, key version and
  a fingerprint of both public keys in the daemon's credential file. A row from the relay is
  only a hint. Session keys are wrapped, `/v2/device-keys` answers are accepted, and WebRTC
  offers are verified only for pinned devices whose keys match their pins.
- **Pins come from an approval on this Mac.** `remote.pairing.approve` carries the
  `claim_digest` whose code the user compared with the phone. Desktop sends the digest its dialog
  showed, and `ketplusctl pairing approve TICKET_ID CLAIM_DIGEST` sends the one that `pairing
  status` printed. The daemon reads the ticket again and refuses it unless that digest is
  unchanged and equals
  `base64url(SHA-256(["<ticket>","<device>",<version>,"<agreement>","<signing>"]))` recomputed
  from the claimed keys. This stops the relay from swapping keys and digest together between
  display and approval, and the dialog treats a digest that changes while open as a failed
  pairing. The pin is written after the relay accepts the approval. Revoking a device removes
  all of its pins.
- **One-time adoption, only for existing enrollments.** A new enrollment starts with an empty pin
  set. A Mac that was already enrolled when this change shipped pins the devices active in its
  first device list (trust on first use), so existing pairings keep working. After that, a
  device joins only through an approval on this Mac.
- **Pairing a phone with one Mac does not grant it another Mac's sessions.** Each Mac approves its
  own devices.

## Consequences

- A relay that adds a device after the adoption cannot obtain session keys, sign accepted
  commands or open a peer link to this Mac.
- Devices paired before the upgrade are trusted as the relay listed them at adoption. A relay
  that was already malicious at that moment is not detected.
- Remaining work tracked in #83:
  - #85: a domain-separated machine-challenge signature, which needs a relay change.
  - #88 and #89: a host fingerprint in the pairing QR code, so that clients pin the host as
    well.

## Alternatives considered

- **Workspace-wide trust through host-signed device attestations.** One Mac's approval would
  admit a device on every Mac in the workspace. This needs new relay storage and an API, plus
  cross-Mac key continuity. Deferred: approving on each Mac is simpler, and nothing is lost when
  a Mac is removed.
- **No adoption, re-pair everything.** Safest, but it would disconnect every existing phone at
  upgrade.
