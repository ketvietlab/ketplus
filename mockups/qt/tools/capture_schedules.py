#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Capture the scheduling proposal offscreen; reject incorrect startup evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

CASES = [
    ("01-schedules", ["--screen=schedules"]),
    ("01-view-menu", ["--screen=workspace", "--open=view-menu"]),
    ("02-editor-team", ["--screen=schedules", "--state=edit"]),
    ("02-editor-new-conversation", ["--screen=schedules", "--state=new-conversation"]),
    ("02-editor-weekly", ["--screen=schedules", "--state=weekly"]),
    ("02-editor-policies", ["--screen=schedules", "--state=policies"]),
    ("02-editor-validation", ["--screen=schedules", "--state=validation"]),
    ("03-history-failure", ["--screen=schedules", "--state=failure"]),
    ("03-delete", ["--screen=schedules", "--state=delete"]),
    ("04-new-send-now", ["--screen=workspace", "--state=schedule-new"]),
    ("04-new-at-time", ["--screen=workspace", "--state=schedule-at"]),
    ("04-new-repeat-more", ["--screen=workspace", "--state=schedule-more"]),
    ("04-new-weekly", ["--screen=workspace", "--state=schedule-weekly"]),
    ("04-scheduled-send", ["--screen=workspace", "--state=schedule-ready"]),
    ("04-existing-schedule", ["--screen=workspace", "--state=schedule-existing"]),
    ("05-scheduled-transcript", ["--screen=workspace", "--state=scheduled"]),
    ("06-failure-toast", ["--screen=workspace", "--state=schedule-failure"]),
    ("07-team-now", ["--screen=teams", "--open=new-team"]),
    ("07-team-start-dialog", ["--screen=teams", "--open=new-team-scheduled"]),
    ("07-team-repeat", ["--screen=teams", "--open=new-team-repeat"]),
    ("07-team-members", ["--screen=teams", "--open=new-team-members"]),
    ("07-team-start-invalid", ["--screen=teams", "--open=new-team-start-invalid"]),
    ("08-team-waiting", ["--screen=teams", "--state=scheduled"]),
    ("08-team-recurring", ["--screen=teams", "--state=recurring"]),
    ("08-team-updates", ["--screen=teams", "--state=recurring", "--scroll=1"]),
    ("09-empty", ["--screen=schedules", "--state=empty"]),
    ("10-minimum", ["--screen=schedules", "--size=900x600"]),
    ("10-minimum-editor", ["--screen=schedules", "--state=new-conversation", "--size=900x600"]),
    ("10-minimum-popover", ["--screen=workspace", "--state=schedule-more", "--size=1024x640"]),
    ("10-minimum-scheduled-send", ["--screen=workspace", "--state=schedule-ready", "--size=1024x640"]),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "evidence" / "schedules")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    expected = {f"{name}-{theme}.png" for name, _ in CASES for theme in ("light", "dark")}
    # Remove only this index's obsolete generated captures, never unrelated artifacts.
    for old in args.output.glob("*.png"):
        if old.name not in expected:
            old.unlink()
    manifest = []
    for name, options in CASES:
        for theme in ("light", "dark"):
            target = args.output / f"{name}-{theme}.png"
            flags = [*options, f"--{theme}"]
            result = subprocess.run([str(args.binary.resolve()), *flags, f"--screenshot={target.resolve()}"],
                                    env=dict(os.environ, QT_QPA_PLATFORM="offscreen"), capture_output=True, text=True)
            warnings = [line for line in result.stderr.splitlines()
                        if any(word in line for word in ("QML", "qrc:", "TypeError", "ReferenceError"))]
            if result.returncode or warnings:
                raise SystemExit(f"{target.name}: capture failed ({result.returncode})\n" + "\n".join(warnings))
            content = target.read_bytes()
            width, height = struct.unpack(">II", content[16:24])
            manifest.append(dict(file=target.name, flags=flags, width=width, height=height,
                                 sha256=hashlib.sha256(content).hexdigest()))
            print(target.name, flush=True)
    by_file = {item["file"]: item for item in manifest}
    for theme in ("light", "dark"):
        for a, b in (("09-empty", "01-schedules"), ("02-editor-policies", "02-editor-team")):
            if by_file[f"{a}-{theme}.png"]["sha256"] == by_file[f"{b}-{theme}.png"]["sha256"]:
                raise SystemExit(f"Incorrect duplicate evidence: {a} / {b} / {theme}")
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Verified {len(manifest)} captures; empty/policies differ from list/editor.")


if __name__ == "__main__":
    main()
