# WebRTC transport spike

Phase 1 of the WebRTC migration (ADR 0024, "WebRTC transport and a forward-only
relay") asks whether data channels actually carry KetPlus traffic over each path
the ADR orders (direct, TURN over UDP, TURN over TCP/TLS, then the HTTPS relay),
with a stack that fits the daemon's build and licences and talks to the
libwebrtc stack the iOS client uses. This records what the spike in
`benchmarks/webrtc-datachannel` and its iOS client in the app repository
(`apps/ios/Spikes/WebRtcDataChannel` in `ketplus-app`) showed on 2026-10-12.
The figures are in
`benchmarks/results/2026-10-12-webrtc-datachannel-macos-m1-pro.md`.

Nothing here ships. The spike is a standalone CMake project that the product
build neither includes nor links.

## What the spike does

`ketplus_webrtc_spike` uses libdatachannel 0.24.6 without media. Each run opens
a `control` and a `bulk` data channel, then:

- times 300 control round trips at idle;
- sends a pattern-filled bulk transfer, waiting while more than a high-water
  mark is buffered, and checks its SHA-256 at the receiver;
- pings on `control` every 10 ms during the bulk transfer;
- reports the selected candidate pair, the candidates gathered, the maximum
  message size and the process's maximum RSS as one JSON line.

`p2p` and `relay <turn-url>` run both peers in one process. `serve` answers an
offer from `interop.html`, which runs the same exchange in Chromium. Chromium
uses libwebrtc and its dcSCTP data channel stack, as the iOS client does. The
iOS spike runs the same exchange from Google's WebRTC 154 for iOS
(`stasel/WebRTC`), the stack the iOS client will ship. `serve` listens on
127.0.0.1 unless `--bind <address>` names another, such as the LAN address for
a physical phone, and `--verbose` turns on libdatachannel's and libjuice's
verbose log. CI builds the spike and runs its direct and TURN tests on Linux
and macOS (`.github/workflows/webrtc-spike.yml`).

The build pins libjuice to upstream commit `cb52376` (1.7.4 plus libjuice #356)
in place of the 1.7.4 that libdatachannel 0.24.6 pins; see the
recommendation below.

## What worked

| Path | Daemon side | Client side | Outcome |
| --- | --- | --- | --- |
| Direct | libdatachannel + libjuice | libdatachannel + libjuice | Passed: host/udp pair, 64 MiB intact |
| TURN over UDP | libjuice | libjuice | Passed: relayed pair, 64 MiB intact |
| TURN over TCP | libjuice | libjuice | Not possible: libjuice logs "TURN transports TCP and TLS are not supported" and never connects |
| TURN over TCP | libnice | libnice | Passed: coturn accepted TCP connections, the pair crossed the relay, 64 MiB intact |
| Direct | libjuice | Chromium 154 | Passed: 32 MiB intact each way, both ends advertise a 262,144-byte maximum message |
| TURN over UDP | libjuice, all candidates | Chromium, relay only | Passed: Chromium relayed over UDP to the host's direct address, 32 MiB intact each way |
| TURN over TCP | libjuice | Chromium, relay only over TCP | Not verified in Chromium: the KetPlus browser session's origin proxy blocked Chromium's TURN TCP connection. The policy was not bypassed. The iOS rows cover libwebrtc over TURN TCP |
| Direct | libjuice | iOS WebRTC 154, simulator | Passed: host/udp pair, 32 MiB intact each way |
| TURN over UDP | libjuice | iOS, relay only | Passed: iOS relayed over UDP to the host's direct address, 32 MiB intact each way |
| TURN over TCP | libjuice 1.7.4 | iOS, relay only over TCP | Failed: libjuice answered every connectivity check with 400 Bad Request (zero tiebreaker, below) |
| TURN over TCP | libjuice `cb52376` | iOS, relay only over TCP | Passed: iOS reached TURN over TCP, the server relayed to the host over UDP, 32 MiB intact each way |
| TURN over TLS | libjuice `cb52376` | iOS, relay only over TLS | Passed with a self-signed certificate that the spike does not validate, 32 MiB intact each way |

TURN over TCP or TLS concerns only one peer's leg to the TURN server. The
server always relays to the other peer over UDP. A client on a network that
blocks UDP (hotel, corporate or mobile carrier Wi-Fi) reaches TURN over TCP or
TLS from libwebrtc, while the daemon keeps UDP to the same server. The libnice
TURN TCP run selected exactly that shape: one end reached the other end's
TCP-allocated relay from a host UDP address. So did every iOS TURN TCP and TLS
run: the phone's relayed candidate, allocated over TCP or TLS, paired with the
daemon's host UDP address. The daemon itself needs TURN over
TCP only when the daemon's own network blocks UDP, and in that case ADR 0024
already falls back to the HTTPS relay.

## Recommendation for the daemon

Use libdatachannel with libjuice, gather every candidate type, and leave TURN
over TCP/TLS to clients. Ship a libjuice that accepts a zero ICE tiebreaker:

- libwebrtc's TURN-over-TCP ports send ICE-CONTROLLING with the value 0. RFC
  8445 allows any 64-bit tiebreaker, but libjuice 1.7.4 takes a zero value for
  an absent attribute and answers the check with 400 Bad Request, so an iOS
  client that can reach TURN only over TCP never connects. A diagnostic build
  saw 0 in all 16 rejected checks.
- libjuice #356, merged upstream on 2026-10-06 and not yet released, records
  attribute presence separately. Commit `cb52376` is 1.7.4 plus that change
  only, and the spike pins it. The daemon must pin it too until a
  libdatachannel release pins libjuice 1.7.5 or later.

Do not adopt libnice:

- It needs GLib, and under LGPL-2.1 (or MPL-1.1) it must be linked
  dynamically. The libjuice build links only OpenSSL beyond the system, and the
  daemon already requires OpenSSL 3.
- 3 of 30 repeated libnice test runs crashed in teardown (SIGSEGV, a null
  dereference in libnice's `component_io_cb` on the GLib main loop thread), and
  earlier manual runs crashed the same way. One libnice direct run did not open
  within 15 seconds; eight repeats did. libjuice did not crash or fail to open
  in its 30 repeated test runs or in any other run.
- libnice took 100–125 ms to open a direct connection and 0.75–2.3 s through
  TURN. libjuice took 9–11 ms and 27–73 ms on the same machine.

libwebrtc on the daemon was not built. Its checkout and build need far more
than this machine's free disk, and it brings a gn/ninja toolchain, BoringSSL and
media code. Chromium's libwebrtc interoperated with libdatachannel without any
adaptation, so the daemon does not need libwebrtc for compatibility.

If adopted, the licences are libdatachannel and libjuice MPL-2.0, usrsctp
BSD-3-Clause and plog MIT, all linked statically. The pinned libjuice commit
is unmodified upstream source. `docs/licensing.md` permits
static linking of unmodified MPL-2.0 files in a proprietary larger work. Add
them to its dependency table, with notices, in the change that first ships
them.

## Findings that shape the protocol

1. **Separate channels do not isolate latency.** All channels share one SCTP
   association. Control pings waited behind bulk data:

   | Configuration | Idle p50 | p50 during bulk |
   | --- | --- | --- |
   | In process, 4 MiB high-water mark | 0.1 ms | 110–410 ms |
   | In process, 256 KiB–1 MiB high-water mark | 0.1 ms | 50–165 ms |
   | Against Chromium, 1 MiB high-water mark | 0.1 ms | 50 ms |
   | Against iOS, direct or TURN UDP, 1 MiB high-water mark | 0.2–0.4 ms | 41–56 ms |
   | Against iOS, TURN TCP or TLS, 1 MiB high-water mark | 0.2–0.7 ms | 25–130 ms, p95 up to 0.9 s |

   Below 256 KiB the in-process figure stopped improving (64 KiB: 100 ms),
   because usrsctp keeps its own 1 MiB send buffer.

   TURN over TCP or TLS adds the TCP leg's head-of-line blocking: one TCP run's
   control RTT reached 0.9 s at p95 during bulk, the other two 65–122 ms.

   The transport adapter must therefore keep bulk sends to a small buffered
   amount and let control and event messages go first. Large file or diff
   transfers have to yield to them. Two options remain to measure on a real
   network rather than loopback: a smaller usrsctp send buffer
   (`rtc::SctpSettings::sendBufferSize`), and bulk on a second peer connection.
2. **Message size.** Both stacks advertise 262,144 bytes. Without SCTP I-DATA,
   one message holds the association until it is sent, so bulk messages should
   stay at 64 KiB or less.
3. **Throughput and memory are not the constraint.** On loopback, libdatachannel
   moved 12–65 MiB/s in process and 24–33 MiB/s against Chromium. The loopback
   runs are CPU bound, so the numbers only show that the stack is not the
   bottleneck for KetPlus event volume. A whole spike process (two peers and a
   64 MiB transfer) peaked at 14–29 MiB RSS.
4. **A relay-only policy does not force a relayed local candidate.** Under a
   relay-only policy, both libjuice and libnice signal only relayed candidates.
   Both may still reach the far end's relay from a local host address.
   Chromium's and iOS WebRTC's relay-only policies did use their own relayed
   candidate. Tests that must prove the relay carried the traffic should check
   that either end of the selected pair is relayed.
5. **Do not wait for gathering to complete.** On iOS the first relayed
   candidate arrived 54–75 ms after the offer was applied, but gathering
   completed only after 1.08–1.13 s, and in one run had not completed after
   15 s. libwebrtc gathers on every interface, here including a VPN tunnel and
   two virtual-machine bridges; its TCP connection to the TURN server over the
   VPN tunnel timed out. The 15-second run's cause was not isolated. The spike
   offered when gathering completed or one second after the first candidate.
   Signaling must carry candidates as they are gathered (trickle ICE), so that
   the slowest interface does not delay the first usable path.

## Still open in phase 1

- The iOS spike on a physical iPhone, on Wi-Fi and LTE rather than the
  simulator and loopback: `serve --bind` with the LAN address, a TURN server the
  phone can reach, and signing with the team. This measures gathering and
  control latency under bulk on real networks.
- TURN over TLS with certificate validation, against the staging TURN server of
  phase 2.
- Capability negotiation and the transport adapter interface, including
  signaling through the relay in place of the spike's local WebSocket.
- A Windows build, once the Windows daemon lane exists.
