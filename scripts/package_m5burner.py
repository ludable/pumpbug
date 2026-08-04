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
import zipfile


VERSION_RE = re.compile(
    r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$"
)
ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)
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
    archive_name: str


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
            "firmware/bootloader_0x0000.bin",
        ),
        ImageSpec(
            ImageKey.PARTITIONS,
            "partition table",
            build_dir / "partitions.bin",
            0x8000,
            "firmware/partitions_0x8000.bin",
        ),
        ImageSpec(
            ImageKey.BOOT_APP,
            "Arduino boot application",
            # PlatformIO flashes boot_app0 even with this factory-only layout;
            # include it so the package reproduces PlatformIO's upload set.
            framework_dir / "tools/partitions/boot_app0.bin",
            0xE000,
            "firmware/boot_app0_0xe000.bin",
        ),
        ImageSpec(
            ImageKey.APPLICATION,
            "Pump Bug application",
            build_dir / "firmware.bin",
            layout.app_offset,
            f"firmware/pump-bug_0x{layout.app_offset:x}.bin",
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


def load_metadata(path: Path, version: str) -> dict:
    metadata = json.loads(path.read_text(encoding="utf-8"))
    required_strings = (
        "name",
        "description",
        "keywords",
        "author",
        "repository",
        "framework",
    )
    for key in required_strings:
        if not isinstance(metadata.get(key), str):
            raise ValueError(f"metadata field {key!r} must be a string")

    category = metadata.get("firmware_category")
    if not isinstance(category, dict):
        raise ValueError("metadata field 'firmware_category' must be an object")
    if category.get("path") != "firmware":
        raise ValueError("firmware_category.path must be 'firmware'")
    if category.get("device") != ["M5StickS3"]:
        raise ValueError("firmware_category.device must contain only M5StickS3")
    if category.get("default_baud") != 921600:
        raise ValueError("firmware_category.default_baud must be 921600")

    metadata["version"] = version
    return metadata


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=True) + "\n").encode("utf-8")


def write_zip(
    path: Path,
    metadata: dict,
    images: Iterable[tuple[ImageSpec, bytes]],
) -> None:
    members = [("m5burner.json", json_bytes(metadata))]
    members.extend((spec.archive_name, data) for spec, data in images)

    with zipfile.ZipFile(path, "w") as archive:
        for name, data in members:
            info = zipfile.ZipInfo(name, ZIP_TIMESTAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data, compresslevel=9)


def verify_zip(
    path: Path,
    metadata: dict,
    images: Iterable[tuple[ImageSpec, bytes]],
) -> None:
    with zipfile.ZipFile(path) as archive:
        expected = {"m5burner.json": json_bytes(metadata)}
        expected.update({spec.archive_name: data for spec, data in images})
        if archive.namelist() != list(expected):
            raise RuntimeError("M5Burner ZIP member list verification failed")
        for name, data in expected.items():
            if archive.read(name) != data:
                raise RuntimeError(f"M5Burner ZIP verification failed for {name}")


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
    metadata_path: Path,
    output_dir: Path,
) -> tuple[Path, Path, Path]:
    version = read_version(project_root / "VERSION")
    layout = read_flash_layout(project_root, build_dir)
    images = read_images(image_specs(build_dir, framework_dir, layout))
    merged = merge_images(images, layout)
    metadata = load_metadata(metadata_path, version)

    output_dir.mkdir(parents=True, exist_ok=True)
    merged_name = f"pump-bug-{version}.bin"
    zip_name = f"pump-bug-{version}-m5burner.zip"
    merged_path = output_dir / merged_name
    zip_path = output_dir / zip_name
    checksums_path = output_dir / "SHA256SUMS"
    with tempfile.TemporaryDirectory(prefix=".m5burner-", dir=output_dir) as temp:
        temp_dir = Path(temp)
        merged_temp = temp_dir / merged_name
        zip_temp = temp_dir / zip_name
        checksums_temp = temp_dir / "SHA256SUMS"
        merged_temp.write_bytes(merged)
        write_zip(zip_temp, metadata, images)

        if merged_temp.read_bytes() != merged:
            raise RuntimeError("merged image verification failed")
        verify_zip(zip_temp, metadata, images)
        checksum_text = "".join(
            f"{sha256(path)}  {path.name}\n" for path in (merged_temp, zip_temp)
        )
        checksums_temp.write_text(checksum_text, encoding="ascii")

        # A failed publication must not leave old sums beside new artifacts.
        checksums_path.unlink(missing_ok=True)
        os.replace(merged_temp, merged_path)
        os.replace(zip_temp, zip_path)
        os.replace(checksums_temp, checksums_path)

    return merged_path, zip_path, checksums_path


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
        "--metadata",
        type=Path,
        help="source metadata JSON (default: packaging/m5burner/metadata.json)",
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
    metadata_path = (
        args.metadata or project_root / "packaging/m5burner/metadata.json"
    ).resolve()
    output_dir = (args.output_dir or project_root / "dist").resolve()

    artifacts = build_artifacts(
        project_root, build_dir, framework_dir, metadata_path, output_dir
    )
    for artifact in artifacts:
        if artifact.is_relative_to(project_root):
            print(artifact.relative_to(project_root))
        else:
            print(artifact)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
