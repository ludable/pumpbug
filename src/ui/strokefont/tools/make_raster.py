#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ludable
# SPDX-License-Identifier: AGPL-3.0-only

"""Compile and run the strokefont raster inspector.

Renders a string at a pixel height as a host pixel grid with device-matching
arc/body geometry (verbatim port of lgfx's fill_arc_helper). Caps and diagonals
are modeled closely but are not bit-exact. Arguments after the script are
forwarded to the tool.

    python make_raster.py "g" 24
    python make_raster.py "bar" 20 --svg /tmp/bar.svg --scale 12
    python make_raster.py --file glyph.txt --rows 5 --context gs
    python make_raster.py --file glyph.txt --watch
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path


def main() -> int:
    here = Path(__file__).resolve().parent
    source = here / "strokefont_raster.cpp.in"
    build_dir = here / ".build"
    binary = build_dir / "strokefont_raster"
    build_dir.mkdir(parents=True, exist_ok=True)

    compile_cmd = [
        os.environ.get("CXX", "c++"),
        "-std=c++14",
        "-x",
        "c++",
        "-O2",
        "-Wall",
        "-Wextra",
        str(source),
        "-o",
        str(binary),
    ]
    subprocess.run(compile_cmd, check=True)

    args = sys.argv[1:]
    watch = "--watch" in args
    args = [a for a in args if a != "--watch"]

    if not watch:
        return subprocess.run([str(binary), *args]).returncode

    # Watch mode: re-run when the art file changes.
    watch_path: Path | None = None
    for i, a in enumerate(args):
        if a == "--file" and i + 1 < len(args):
            watch_path = Path(args[i + 1])
            break

    if watch_path is None or str(watch_path) == "-":
        print("error: --watch requires --file PATH", file=sys.stderr)
        return 1

    last_mtime = 0.0
    try:
        while True:
            try:
                mtime = watch_path.stat().st_mtime
            except FileNotFoundError:
                mtime = 0.0
            if mtime != last_mtime:
                last_mtime = mtime
                print(f"\n--- {watch_path} changed ---")
                subprocess.run([str(binary), *args])
            time.sleep(0.2)
    except KeyboardInterrupt:
        print("\nwatch stopped")
        return 0


if __name__ == "__main__":
    sys.exit(main())
