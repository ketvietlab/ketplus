#!/usr/bin/env python3
"""Synthetic process protocol only: no server, renderer, network or provider."""
import os
from pathlib import Path
import signal
import sys
import time

if sys.argv[1:] == ["--version"]:
    print("0.5.2")
    raise SystemExit(0)

port = sys.argv[sys.argv.index("--port") + 1]
banner = f"KetAtlas: http://127.0.0.1:{port}"
mode = os.environ["KETPLUS_TEST_ATLAS_READY_MODE"]
if mode == "on-stop":
    def stopped(_signum, _frame):
        print(banner, flush=True)
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, stopped)
    Path(os.environ["KETPLUS_TEST_ATLAS_READY_MARKER"]).touch()
elif mode == "valid-split":
    sys.stdout.write(banner[:-1])
    sys.stdout.flush()
    time.sleep(0.05)
    print(banner[-1:], flush=True)
else:
    messages = {
        "wrong-port": f"KetAtlas: http://127.0.0.1:{int(port) % 65535 + 1}\n",
        "longer-port": banner + "0\n",
        "embedded": "log prefix: " + banner + "\n",
        "unterminated": banner,
        "truncated-prefix": "x" * 32768 + banner + "\n" + "x" * (32768 - len(banner) - 1),
        "valid-lf": banner + "\n",
        "valid-crlf": banner + "\r\n",
        "valid-slash": banner + "/ \t\n",
        "valid-after-log": "x" * 40000 + "\n" + banner + "\n",
        "valid-exit": banner + "\n",
        "wrong-then-right": banner + "0\n" + banner + "\n",
    }
    sys.stdout.write(messages[mode])
    sys.stdout.flush()
    if mode == "valid-exit" or not (mode.startswith("valid-") or mode == "wrong-then-right"):
        raise SystemExit(0)

while True:
    signal.pause()
