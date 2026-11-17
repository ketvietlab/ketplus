#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Record renderer opt-in without executing a renderer, browser or provider."""
import json
import os
from pathlib import Path
import signal
import sys
import time

record = Path(os.environ["KETPLUS_ATLAS_FIXTURE_RECORD"])
if sys.argv[1:] == ["--version"]:
    if os.environ.get("KETPLUS_ATLAS_FIXTURE_HOLD_VERSION") == "1":
        (record / "version-started").touch()
        while not (record / "version-continue").exists():
            time.sleep(0.01)
    print("0.5.2")
    raise SystemExit(0)

(record / "argv.json").write_text(json.dumps(sys.argv[1:]))
if "--renderer" in sys.argv:
    (record / "renderer-requested").touch()
port = sys.argv[sys.argv.index("--port") + 1]
print(f"KetAtlas: http://127.0.0.1:{port}", flush=True)
while True:
    signal.pause()
