# WebRTC data channel spike — 2026-10-12

Findings and recommendation: `docs/webrtc-transport-spike.md`.

## Environment

- Source: `benchmarks/webrtc-datachannel` on `spike/webrtc-datachannel`
- Build: Release, AppleClang, OpenSSL 3.6.3
- libdatachannel 0.24.6 (`6b1e2e6`), no media; ICE backend libjuice (bundled
  1.7.4 `b89c792`, or `cb52376` = 1.7.4 plus libjuice #356 where noted) or
  libnice 0.1.24 (Homebrew, with GLib)
- TURN: coturn 4.18.0 listening on 127.0.0.1:3479, long-term credentials, and
  for the iOS TLS runs on 127.0.0.1:5349 with a throwaway self-signed
  certificate. It relayed over the Mac's LAN address unless noted
- Browser: HeadlessChrome 154 driven through the KetPlus browser tools
- iOS: the ketplus-app spike (`apps/ios/Spikes/WebRtcDataChannel`) with
  `stasel/WebRTC` 154.0.0 on an iPhone 17 Pro simulator, iOS 26.5
- OS: macOS 26.2, Darwin 25.2.0
- Hardware: Apple M1 Pro, 10 CPU cores, 32 GiB RAM

Every run is on one machine over loopback or the LAN address. Throughput is CPU
bound, and latency during bulk varies by a factor of two to three between runs.
Read the figures as reachability and order of magnitude, not network
performance.

## In process (two peers, 64 MiB bulk)

`ketplus_webrtc_spike p2p|relay <url> --bulk-mib 64 [--high-water-kib N]`

| Backend and path | Selected pair (local → remote) | Open | Idle RTT p50 / p99 | High water | Bulk | SHA-256 | RTT during bulk p50 / p99 | Max RSS |
| --- | --- | ---: | ---: | ---: | ---: | --- | ---: | ---: |
| libjuice direct | host/udp → host/udp | 11 ms | 0.10 / 0.18 ms | 4 MiB | 44.8 MiB/s | match | 410 / 997 ms | 20 MiB |
| libjuice direct | host/udp → host/udp | 9 ms | 0.08 / 0.25 ms | 256 KiB | 43.7 MiB/s | match | 165 / 560 ms | 14 MiB |
| libjuice TURN UDP | relay/udp → relay/udp | 73 ms | 0.50 / 3.04 ms | 4 MiB | 12.1 MiB/s | match | 739 / 1972 ms | 24 MiB |
| libjuice TURN UDP | host/udp → relay/udp | 27 ms | 0.36 / 1.92 ms | 256 KiB | 12.0 MiB/s | match | 173 / 577 ms | 16 MiB |
| libjuice TURN UDP, relay on loopback | relay/udp → relay/udp | 59 ms | 0.10 / 0.46 ms | 1 MiB | 54.0 MiB/s | match | 78 / 437 ms | 16 MiB |
| libjuice TURN TCP | none (not supported) | — | — | — | — | — | — | 8 MiB |
| libnice direct | host/udp → host/udp | 100–125 ms | 0.09 / 0.22 ms | 1 MiB | 61.4 MiB/s | match | 180 / 481 ms | 25 MiB |
| libnice TURN UDP | host/udp → relay/udp | 783 ms | 0.11 / 0.34 ms | 4 MiB | 65.6 MiB/s | match | 96 / 183 ms | 29 MiB |
| libnice TURN TCP | host/udp → relay/udp | 747 ms | 0.12 / 0.31 ms | 4 MiB | 54.4 MiB/s | match | 616 / 1203 ms | 28 MiB |

Control latency during bulk against the high-water mark, libjuice direct,
in one series:

| High water | Bulk | RTT during bulk p50 / p95 / p99 |
| ---: | ---: | ---: |
| 4 MiB | 47.1 MiB/s | 111 / 242 / 266 ms |
| 1 MiB | 45.8 MiB/s | 67 / 133 / 163 ms |
| 256 KiB | 45.6 MiB/s | 50 / 166 / 226 ms |
| 64 KiB | 45.4 MiB/s | 101 / 276 / 325 ms |

Reliability over ten repeats of each test set (30 runs per backend): libjuice
had no failures. libnice had 3 SIGSEGV in teardown, inside `component_io_cb` on
the GLib main loop thread.

## Against Chromium (32 MiB each way, host high water 1 MiB)

`ketplus_webrtc_spike serve 8765 --high-water-kib 1024`, then
`interop.html?mib=32[&relay=udp|tcp]` served from `http://127.0.0.1:8766`.

| Path | Chromium pair | Host pair | Open | Idle RTT p50 / p99 | Upload | Download | SHA-256 | RTT during download p50 / p95 / p99 | Host max RSS |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- | ---: | ---: |
| Direct | host/udp → host/udp | host/udp → host/udp | 226 ms | 0.1 / 0.7 ms | 24.5 MiB/s | 31.9 MiB/s | match both ways | 51 / 67 / 69 ms | 22 MiB |
| Chromium relay only, TURN UDP | relay/udp (TURN over UDP) → host/udp | host/udp → relay/udp | 219 ms | 0.2 / 0.4 ms | 28.5 MiB/s | 33.4 MiB/s | match both ways | 48 / 60 / 61 ms | 17 MiB |
| Chromium relay only, TURN TCP | not reached | — | — | — | — | — | — | — | — |

Both ends advertised a maximum message size of 262,144 bytes. The TURN TCP run
did not connect: the browser session's origin proxy blocked Chromium's TCP
connection to the TURN server (13 blocked requests). The first TURN UDP attempt
failed while coturn relayed only over loopback, because a loopback relay cannot
reach the host's LAN candidates. The run above relayed over the LAN address.

## Against iOS libwebrtc (32 MiB each way, host high water 1 MiB)

`ketplus_webrtc_spike serve 8765 --high-water-kib 1024`, then the iOS spike's
`testAgainstSpikeHost` with `KETPLUS_SPIKE_RELAY` unset, `udp`, `tcp` or `tls`.
TURN rows are relay only on iOS. The client offers when gathering completes or
one second after its first candidate, whichever is sooner.

| Path | Host libjuice | Runs | iOS pair | Host pair | Open | Upload | Download | SHA-256 | RTT during download p50 / p95 / max |
| --- | --- | ---: | --- | --- | ---: | ---: | ---: | --- | ---: |
| Direct | 1.7.4 and `cb52376` | 3 | host/udp → host/udp | prflx/udp → host/udp | 0.17–0.53 s | 33–39 MiB/s | 29–39 MiB/s | match both ways | 41–56 / 68–103 / 79–124 ms |
| TURN UDP | `cb52376` | 3 | relay/udp, TURN over UDP → host/udp | host/udp → relay/udp | 1.17–1.21 s | 52–56 MiB/s | 36–39 MiB/s | match both ways | 41–46 / 61–74 / 87–93 ms |
| TURN TCP | 1.7.4 | 2 | none | none | — | — | — | — | — |
| TURN TCP | `cb52376` | 3 | relay/udp, TURN over TCP → host/udp | host/udp → relay/udp | 1.14–1.18 s | 30–54 MiB/s | 22–39 MiB/s | match both ways | 25–130 / 65–906 / 95–963 ms |
| TURN TLS | `cb52376` | 3 | relay/udp, TURN over TLS → host/udp | host/udp → relay/udp | 1.18–1.28 s | 39–55 MiB/s | 34–41 MiB/s | match both ways | 39–118 / 47–346 / 53–388 ms |

With libjuice 1.7.4 every iOS check over TURN TCP was answered with 400 Bad
Request, logged as "Controlling and controlled attributes mismatch in request".
A diagnostic build showed the ICE-CONTROLLING value was 0 in all 16 rejected
checks. The same exchange from macOS (`swift test`) failed the same way and
passed with `cb52376`. TLS was run only with `cb52376`.

Gathering in the relayed runs, from applying the offer: the first candidate
after 54–75 ms, complete after 1.08–1.13 s. In one earlier TURN UDP run
gathering had not completed after 15 s, although coturn logged all three
allocations as granted. libwebrtc tried every interface, including a VPN tunnel
and two virtual-machine bridges. Its TCP connection over the VPN tunnel to the
loopback TURN address failed with a timeout.

Idle control round trips had a p50 of 0.16–0.75 ms and a p99 of 0.4–11 ms,
except the first direct run (2.1 / 37 ms). One TCP run's RTT during the
download reached 0.9 s at p95, against 65–122 ms in the other two.
