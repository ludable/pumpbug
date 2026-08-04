#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ludable
# SPDX-License-Identifier: AGPL-3.0-only

"""Compile and run the C++ strokefont SVG contact-sheet generator."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


def default_compiler() -> str:
    return os.environ.get("CXX", "c++")


def main() -> int:
    here = Path(__file__).resolve().parent
    strokefont_dir = here.parent

    parser = argparse.ArgumentParser(
        description=(
            "Build the C++ contact-sheet generator and render "
            "strokefont_contact_sheet.svg."
        )
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=strokefont_dir / "strokefont_contact_sheet.svg",
        help="SVG output path.",
    )
    parser.add_argument(
        "--compiler",
        default=default_compiler(),
        help="C++ compiler to use, defaults to $CXX or c++.",
    )
    parser.add_argument(
        "--keep-binary",
        action="store_true",
        help="Leave the compiled generator in tools/.build/.",
    )
    args = parser.parse_args()

    source = here / "strokefont_contact_sheet.cpp.in"
    build_dir = here / ".build"
    binary = build_dir / "strokefont_contact_sheet"
    output = args.output.resolve()

    build_dir.mkdir(parents=True, exist_ok=True)
    output.parent.mkdir(parents=True, exist_ok=True)

    compile_cmd = [
        args.compiler,
        "-std=c++17",
        "-x",
        "c++",
        "-O2",
        "-Wall",
        "-Wextra",
        "-pedantic",
        str(source),
        "-o",
        str(binary),
    ]
    subprocess.run(compile_cmd, check=True)
    subprocess.run([str(binary), str(output)], check=True)

    if not args.keep_binary:
        try:
            binary.unlink()
        except FileNotFoundError:
            pass

    print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
