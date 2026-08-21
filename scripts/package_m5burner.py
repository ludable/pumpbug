#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ludable
# SPDX-License-Identifier: AGPL-3.0-only

"""Build reproducible M5Burner artifacts from the release firmware images."""

from __future__ import annotations

import argparse
from enum import Enum
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from typing import Iterable, NamedTuple, Optional


VERSION_RE = re.compile(
    r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$"
)
PARTITION_ENTRY = struct.Struct("<HBBII16sI")
PARTITION_MAGIC = 0x50AA


class ImageKey(Enum):
    BOOTLOADER = "bootloader"
    PARTITIONS = "partitions"
    BOOT_APP = "boot_app"
    APPLICATION = "application"


class ImageSpec(NamedTuple):
    key: ImageKey
    label: str
    source: Path
    offset: int


class FlashLayout(NamedTuple):
    flash_size: int
    app_offset: int
    app_size: int


def read_version(path: Path) -> str:
    version = path.read_text(encoding="utf-8").strip()
    if not VERSION_RE.fullmatch(version):
        raise ValueError(f"invalid firmware version in {path}: {version!r}")
    return version


def find_platformio(project_root: Path) -> Path:
    configured = os.environ.get("PLATFORMIO_EXE")
    if configured:
        candidates = [Path(configured).expanduser()]
    else:
        on_path = shutil.which("pio") or shutil.which("platformio")
        candidates = ([Path(on_path)] if on_path else []) + [
            project_root / ".venv/bin/pio",
            project_root.parent / ".venv/bin/pio",
            Path.home() / ".platformio/penv/bin/pio",
        ]

    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    raise FileNotFoundError(
        "PlatformIO was not found; activate its environment or set PLATFORMIO_EXE"
    )


def platformio_core_dir(platformio: Path, project_root: Path) -> Path:
    configured = os.environ.get("PLATFORMIO_CORE_DIR")
    if configured:
        return Path(configured).expanduser().resolve()

    result = subprocess.run(
        [str(platformio), "system", "info", "--json-output"],
        cwd=project_root,
        check=True,
        capture_output=True,
        text=True,
    )
    info = json.loads(result.stdout)
    return Path(info["core_dir"]["value"]).expanduser().resolve()


def parse_flash_size(value: object) -> int:
    if not isinstance(value, str):
        raise ValueError("board upload.flash_size must be a string")
    match = re.fullmatch(r"([1-9][0-9]*)(KB|MB)", value)
    if not match:
        raise ValueError(f"unsupported board flash size: {value!r}")
    multiplier = 1024 if match.group(2) == "KB" else 1024 * 1024
    return int(match.group(1)) * multiplier


def factory_partition(data: bytes) -> tuple[int, int]:
    matches = []
    end = len(data) - PARTITION_ENTRY.size + 1
    for offset in range(0, end, PARTITION_ENTRY.size):
        magic, part_type, subtype, part_offset, size, _label, _flags = (
            PARTITION_ENTRY.unpack_from(data, offset)
        )
        if magic != PARTITION_MAGIC:
            break
        if part_type == 0x00 and subtype == 0x00:
            matches.append((part_offset, size))

    if len(matches) != 1:
        raise ValueError("partition table must contain exactly one factory app")
    app_offset, app_size = matches[0]
    if app_offset <= 0 or app_size <= 0:
        raise ValueError("factory app partition has invalid bounds")
    return app_offset, app_size


def read_flash_layout(project_root: Path, build_dir: Path) -> FlashLayout:
    board_path = project_root / "boards/m5stick-s3.json"
    board = json.loads(board_path.read_text(encoding="utf-8"))
    try:
        flash_size = parse_flash_size(board["upload"]["flash_size"])
    except KeyError as exc:
        raise ValueError(f"missing flash size in {board_path}") from exc

    partitions_path = build_dir / "partitions.bin"
    try:
        partitions = partitions_path.read_bytes()
    except FileNotFoundError as exc:
        raise FileNotFoundError(
            f"missing partition table: {partitions_path}"
        ) from exc
    if not partitions.startswith(b"\xaa\x50"):
        raise ValueError("partitions.bin is not an ESP partition table")
    app_offset, app_size = factory_partition(partitions)
    if app_offset + app_size > flash_size:
        raise ValueError("factory app partition extends beyond device flash")
    return FlashLayout(flash_size, app_offset, app_size)


def image_specs(
    build_dir: Path, framework_dir: Path, layout: FlashLayout
) -> list[ImageSpec]:
    return [
        ImageSpec(
            ImageKey.BOOTLOADER,
            "bootloader",
            build_dir / "bootloader.bin",
            0x0000,
        ),
        ImageSpec(
            ImageKey.PARTITIONS,
            "partition table",
            build_dir / "partitions.bin",
            0x8000,
        ),
        ImageSpec(
            ImageKey.BOOT_APP,
            "Arduino boot application",
            # PlatformIO flashes boot_app0 even with this factory-only layout;
            # include it so the package reproduces PlatformIO's upload set.
            framework_dir / "tools/partitions/boot_app0.bin",
            0xE000,
        ),
        ImageSpec(
            ImageKey.APPLICATION,
            "Pump Bug application",
            build_dir / "firmware.bin",
            layout.app_offset,
        ),
    ]


def read_images(specs: Iterable[ImageSpec]) -> list[tuple[ImageSpec, bytes]]:
    images = []
    for spec in specs:
        try:
            data = spec.source.read_bytes()
        except FileNotFoundError as exc:
            raise FileNotFoundError(f"missing {spec.label}: {spec.source}") from exc
        if not data:
            raise ValueError(f"empty {spec.label}: {spec.source}")
        images.append((spec, data))

    by_key = {spec.key: data for spec, data in images}
    if by_key[ImageKey.BOOTLOADER][0] != 0xE9:
        raise ValueError("bootloader.bin is not an ESP image")
    if by_key[ImageKey.APPLICATION][0] != 0xE9:
        raise ValueError("firmware.bin is not an ESP image")
    if not by_key[ImageKey.PARTITIONS].startswith(b"\xaa\x50"):
        raise ValueError("partitions.bin is not an ESP partition table")
    return images


def merge_images(
    images: Iterable[tuple[ImageSpec, bytes]], layout: FlashLayout
) -> bytes:
    ordered = sorted(images, key=lambda item: item[0].offset)
    end = 0
    for spec, data in ordered:
        if spec.offset < end:
            raise ValueError(f"{spec.label} overlaps the preceding flash image")
        if spec.key == ImageKey.APPLICATION and len(data) > layout.app_size:
            raise ValueError("firmware.bin exceeds the factory app partition")
        end = spec.offset + len(data)
        if end > layout.flash_size:
            raise ValueError(f"{spec.label} extends beyond device flash")

    merged = bytearray(b"\xff" * end)
    for spec, data in ordered:
        merged[spec.offset : spec.offset + len(data)] = data
    return bytes(merged)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_artifacts(
    project_root: Path,
    build_dir: Path,
    framework_dir: Path,
    output_dir: Path,
) -> tuple[Path, Path]:
    version = read_version(project_root / "VERSION")
    layout = read_flash_layout(project_root, build_dir)
    images = read_images(image_specs(build_dir, framework_dir, layout))
    merged = merge_images(images, layout)

    output_dir.mkdir(parents=True, exist_ok=True)
    merged_name = f"pump-bug-{version}.bin"
    merged_path = output_dir / merged_name
    checksums_path = output_dir / "SHA256SUMS"
    with tempfile.TemporaryDirectory(prefix=".m5burner-", dir=output_dir) as temp:
        temp_dir = Path(temp)
        merged_temp = temp_dir / merged_name
        checksums_temp = temp_dir / "SHA256SUMS"
        merged_temp.write_bytes(merged)

        if merged_temp.read_bytes() != merged:
            raise RuntimeError("merged image verification failed")
        checksum_text = f"{sha256(merged_temp)}  {merged_temp.name}\n"
        checksums_temp.write_text(checksum_text, encoding="ascii")

        # A failed publication must not leave old sums beside new artifacts.
        checksums_path.unlink(missing_ok=True)
        os.replace(merged_temp, merged_path)
        os.replace(checksums_temp, checksums_path)

    return merged_path, checksums_path


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="package the existing .pio/build/release images",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        help="release artifact directory (default: .pio/build/release)",
    )
    parser.add_argument(
        "--framework-dir",
        type=Path,
        help="Arduino ESP32 framework package directory (normally auto-detected)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="package output directory (default: dist)",
    )
    return parser.parse_args(argv)


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    project_root = Path(__file__).resolve().parent.parent
    platformio = None
    if not args.skip_build or args.framework_dir is None:
        platformio = find_platformio(project_root)

    if not args.skip_build:
        subprocess.run(
            [str(platformio), "run", "-e", "release"],
            cwd=project_root,
            check=True,
        )

    build_dir = (args.build_dir or project_root / ".pio/build/release").resolve()
    if args.framework_dir:
        framework_dir = args.framework_dir.resolve()
    else:
        core_dir = platformio_core_dir(platformio, project_root)
        framework_dir = core_dir / "packages/framework-arduinoespressif32"
    output_dir = (args.output_dir or project_root / "dist").resolve()

    artifacts = build_artifacts(project_root, build_dir, framework_dir, output_dir)
    for artifact in artifacts:
        if artifact.is_relative_to(project_root):
            print(artifact.relative_to(project_root))
        else:
            print(artifact)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
