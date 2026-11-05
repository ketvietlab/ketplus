# Chat reading anchor checkpoint — 2026-09-11

Measured commit: `491fa78`.

- Apple M1 Pro, 32 GiB RAM, macOS 26.2
- Release build, Qt 6.11.1, Metal
- One process, six resident conversations, 200 synthetic messages each
- No provider calls

```sh
QT_QPA_PLATFORM=cocoa QSG_RHI_BACKEND=metal \
KETPLUS_DISABLE_DAEMON_AUTOSTART=1 \
  bash benchmarks/run-agent-multi-app.sh 1 6 200 \
  benchmarks/results/raw-20260911-chat-reading-anchor.txt
```

| Measurement | Result |
| --- | ---: |
| Warm transcript views | 6 |
| Populate | 236.922 ms |
| Idle CPU | 0.303% |
| Cold-switch CPU | 26.140% |
| Warm-switch CPU | 19.597% |
| Scroll CPU | 88.585% |
| Scroll frame rate | 62.326 FPS |
| Peak RSS | 331,328 KiB |

[Raw output](raw-20260911-chat-reading-anchor.txt) includes startup and aggregate
process samples. This is one checkpoint, not a controlled before/after comparison.
The existing benchmark exercises conversation switching and programmatic scrolling;
mouse/touchpad interaction and preservation of reading positions are covered by
separate deterministic and native pixel tests.
