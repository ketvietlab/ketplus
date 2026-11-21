#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Renders mermaid/*.mmd into assets/mermaid/<name>-<light|dark>@2x.png.

The mock only presents data, so its diagrams are rendered ahead of time, the way the app
renders them at run time (core/src/preview/MermaidRenderer.cpp): mermaid-cli (`mmdc`) with
the bundled configuration in core/resources/mermaid, a transparent background and scale 2.
Run it after editing a .mmd file; it needs `mmdc` on PATH.
"""
import json
import pathlib
import subprocess
import sys
import tempfile

MOCK = pathlib.Path(__file__).resolve().parent.parent
THEMES = MOCK.parent.parent / "core" / "resources" / "mermaid"


def configuration(theme: str) -> dict:
    config = json.loads((THEMES / f"{theme}.json").read_text())
    # MermaidRenderer's mermaidConfiguration() also adds explicit state label fills for Qt
    # SVG. These PNGs come from the browser, which draws the theme's own label colours.
    return config


def main() -> int:
    out = MOCK / "assets" / "mermaid"
    out.mkdir(parents=True, exist_ok=True)
    sources = sorted((MOCK / "mermaid").glob("*.mmd"))
    with tempfile.TemporaryDirectory() as scratch:
        for theme in ("light", "dark"):
            config = pathlib.Path(scratch) / f"{theme}.json"
            config.write_text(json.dumps(configuration(theme)))
            for source in sources:
                target = out / f"{source.stem}-{theme}@2x.png"
                subprocess.run(["mmdc", "-i", str(source), "-o", str(target), "-b", "transparent",
                                "-c", str(config), "-s", "2"], check=True,
                               stdout=subprocess.DEVNULL)
                print(target.relative_to(MOCK))
    return 0


if __name__ == "__main__":
    sys.exit(main())
