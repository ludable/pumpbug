# SPDX-FileCopyrightText: 2026 ludable
# SPDX-License-Identifier: AGPL-3.0-only

"""Check that project-authored source files carry SPDX headers."""

import argparse
from datetime import date
from pathlib import Path
import re
import sys


PROJECT_ROOT = Path(__file__).resolve().parent.parent
COPYRIGHT = f"SPDX-FileCopyrightText: {date.today().year} ludable"
COPYRIGHT_PATTERN = re.compile(
    r"SPDX-FileCopyrightText: 20\d{2}(?:-20\d{2})? ludable"
)
LICENSE = "SPDX-License-Identifier: AGPL-3.0-only"
M5STACK_COPYRIGHT = "SPDX-FileCopyrightText: 2021 M5Stack"
MIXED_LICENSE = "SPDX-License-Identifier: MIT AND AGPL-3.0-only"
LGPL_LICENSE = "SPDX-License-Identifier: LGPL-2.1-or-later"
SOURCE_SUFFIXES = {".cpp", ".css", ".h", ".html", ".in", ".js", ".py"}
GENERATED_PATHS = {
    PROJECT_ROOT / "src/net/embedded_assets.cpp",
    PROJECT_ROOT / "src/net/embedded_assets.h",
}
SPECIAL_REQUIREMENTS = {
    PROJECT_ROOT / "src/util/fft.cpp": (
        COPYRIGHT_PATTERN,
        M5STACK_COPYRIGHT,
        MIXED_LICENSE,
    ),
    PROJECT_ROOT / "src/util/fft.h": (
        COPYRIGHT_PATTERN,
        M5STACK_COPYRIGHT,
        MIXED_LICENSE,
    ),
    PROJECT_ROOT / "variants/m5stack_sticks3/pins_arduino.h": (LGPL_LICENSE,),
}
FORBIDDEN_REQUIREMENTS = {
    PROJECT_ROOT / "variants/m5stack_sticks3/pins_arduino.h": (
        COPYRIGHT_PATTERN,
    ),
}


def source_files():
    files = []
    for root_name in ("scripts", "src", "test", "web-src"):
        root = PROJECT_ROOT / root_name
        files.extend(
            path
            for path in root.rglob("*")
            if path.is_file()
            and path.suffix in SOURCE_SUFFIXES
            and path not in GENERATED_PATHS
        )

    files.extend(
        [
            PROJECT_ROOT / ".github/workflows/ci.yml",
            PROJECT_ROOT / "CMakeLists.txt",
            PROJECT_ROOT / "boards/pump-bug-8MB.csv",
            PROJECT_ROOT / "docs/overview.html",
            PROJECT_ROOT / "platformio.ini",
            PROJECT_ROOT / "test/CMakeLists.txt",
            PROJECT_ROOT / "variants/m5stack_sticks3/pins_arduino.h",
        ]
    )
    return sorted(set(files))


def header_for(path):
    if path in {
        PROJECT_ROOT / "src/util/fft.cpp",
        PROJECT_ROOT / "src/util/fft.h",
    }:
        return (
            f"// {M5STACK_COPYRIGHT}\n"
            f"// {COPYRIGHT}\n"
            f"// {MIXED_LICENSE}\n\n"
        )
    if path == PROJECT_ROOT / "variants/m5stack_sticks3/pins_arduino.h":
        return f"// {LGPL_LICENSE}\n\n"
    if path.suffix == ".css":
        return f"/* {COPYRIGHT} */\n/* {LICENSE} */\n\n"
    if path.suffix == ".html":
        return f"<!-- {COPYRIGHT} -->\n<!-- {LICENSE} -->\n\n"
    if path.name == "platformio.ini":
        marker = ";"
    elif path.suffix in {".cpp", ".h", ".in", ".js"}:
        marker = "//"
    else:
        marker = "#"
    return f"{marker} {COPYRIGHT}\n{marker} {LICENSE}\n\n"


def add_header(path):
    text = path.read_text(encoding="utf-8")
    head = "\n".join(text.splitlines()[:8])
    requirements = SPECIAL_REQUIREMENTS.get(path, (COPYRIGHT_PATTERN, LICENSE))
    if all(requirement_present(requirement, head) for requirement in requirements):
        return False
    if "SPDX-" in head:
        return False

    insert_at = 0
    if text.startswith("#!") or text.lower().startswith("<!doctype html>"):
        newline = text.find("\n")
        insert_at = len(text) if newline < 0 else newline + 1
    updated = text[:insert_at] + header_for(path) + text[insert_at:]
    path.write_text(updated, encoding="utf-8")
    return True


def requirement_present(requirement, text):
    if isinstance(requirement, re.Pattern):
        return requirement.search(text) is not None
    return requirement in text


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--fix", action="store_true", help="add missing headers in place"
    )
    return parser.parse_args()


def main():
    args = parse_args()
    files = source_files()
    if args.fix:
        changed = sum(add_header(path) for path in files)
        print(f"Added SPDX headers to {changed} files")

    missing = []
    forbidden = []
    for path in files:
        text = path.read_text(encoding="utf-8")
        head = "\n".join(text.splitlines()[:8])
        requirements = SPECIAL_REQUIREMENTS.get(
            path, (COPYRIGHT_PATTERN, LICENSE)
        )
        if not all(
            requirement_present(requirement, head)
            for requirement in requirements
        ):
            missing.append(path.relative_to(PROJECT_ROOT))
        if any(
            requirement_present(requirement, text)
            for requirement in FORBIDDEN_REQUIREMENTS.get(path, ())
        ):
            forbidden.append(path.relative_to(PROJECT_ROOT))

    if missing or forbidden:
        if missing:
            print("Missing required SPDX header:", file=sys.stderr)
            for path in missing:
                print(f"  {path}", file=sys.stderr)
        if forbidden:
            print("Forbidden SPDX attribution:", file=sys.stderr)
            for path in forbidden:
                print(f"  {path}", file=sys.stderr)
        return 1
    print(f"SPDX headers verified in {len(files)} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
