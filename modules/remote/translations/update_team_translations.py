#!/usr/bin/env python3
"""Refreshes the team translations from the team sources (T-56, ADR 0124, docs/development.md).

lupdate finds every team text, but it guesses a context per class, and the desktop's
free tr() helpers translate under contexts lupdate cannot see. A team text means the same
wherever the team shows it, so this keeps one context, "Team", which is the context the
translator looks up (modules/remote/src/TeamLanguage.cpp):

    update_team_translations.py [--check] <lupdate> <repository root> <file.ts>...

A translation already written for a text is kept; a text no source uses any more is dropped.
With --check the catalog is not rewritten; missing translations and stale entries fail.
"""

import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

SOURCES = ("apps/desktop/src/teams", "apps/daemon/src/teams")
CONTEXT = "Team"


def sources(root: Path) -> list[str]:
    found = []
    for folder in SOURCES:
        found += sorted(str(p) for p in (root / folder).iterdir() if p.suffix in (".cpp", ".h"))
    return found


def texts(lupdate: str, files: list[str]) -> list[str]:
    with tempfile.TemporaryDirectory() as scratch:
        probe = Path(scratch) / "probe.ts"
        subprocess.run([lupdate, "-silent", "-locations", "none", *files, "-ts", str(probe)],
                       check=True, capture_output=True)
        seen, ordered = set(), []
        for message in ET.parse(probe).getroot().iter("message"):
            source = message.findtext("source") or ""
            if source and source not in seen:
                seen.add(source)
                ordered.append(source)
        return sorted(ordered, key=str.casefold)


def written(ts: Path) -> dict[str, str]:
    if not ts.exists():
        return {}
    result = {}
    for message in ET.parse(ts).getroot().iter("message"):
        translation = message.find("translation")
        if translation is not None and translation.text and translation.get("type") != "unfinished":
            result[message.findtext("source") or ""] = translation.text
    return result


def write(ts: Path, language: str, all_texts: list[str], translated: dict[str, str]) -> int:
    root = ET.Element("TS", version="2.1", language=language)
    context = ET.SubElement(root, "context")
    ET.SubElement(context, "name").text = CONTEXT
    missing = 0
    for source in all_texts:
        message = ET.SubElement(context, "message")
        ET.SubElement(message, "source").text = source
        translation = ET.SubElement(message, "translation")
        if source in translated:
            translation.text = translated[source]
        else:
            translation.set("type", "unfinished")
            missing += 1
    ET.indent(root, space="    ")
    body = ET.tostring(root, encoding="unicode")
    ts.write_text('<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n' + body + "\n",
                  encoding="utf-8")
    return missing


def main() -> int:
    args = sys.argv[1:]
    checking = "--check" in args
    if checking:
        args.remove("--check")
    if len(args) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    lupdate, root, files = args[0], Path(args[1]), [Path(p) for p in args[2:]]
    all_texts = texts(lupdate, sources(root))
    if checking:
        failed = False
        for ts in files:
            translated = written(ts)
            recorded = {message.findtext("source") or "" for message in ET.parse(ts).getroot().iter("message")}
            missing = sorted(set(all_texts) - translated.keys())
            stale = sorted(recorded - set(all_texts))
            for source in missing:
                print(f"{ts.name}: missing translation: {source}", file=sys.stderr)
            for source in stale:
                print(f"{ts.name}: stale source: {source}", file=sys.stderr)
            failed |= bool(missing or stale)
            print(f"{ts.name}: {len(all_texts)} current source texts checked")
        return int(failed)
    for ts in files:
        language = ts.stem.rsplit("_", 1)[-1]
        missing = write(ts, {"vi": "vi_VN"}.get(language, language), all_texts, written(ts))
        print(f"{ts.name}: {len(all_texts)} texts, {missing} without a translation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
