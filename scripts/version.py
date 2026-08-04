# SPDX-FileCopyrightText: 2026 ludable
# SPDX-License-Identifier: AGPL-3.0-only

"""PlatformIO pre-build: compile the repository firmware version into the image."""

import re
from pathlib import Path

Import("env")  # noqa: F821  (PlatformIO injects this)

PROJECT_DIR = Path(env["PROJECT_DIR"])  # noqa: F821
VERSION_FILE = PROJECT_DIR / "VERSION"
VERSION_RE = re.compile(
    r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$"
)

base_version = VERSION_FILE.read_text().strip()
if not VERSION_RE.fullmatch(base_version):
    raise ValueError(
        f"invalid firmware version in {VERSION_FILE}: {base_version!r}"
    )

version = base_version
metadata = env.GetProjectOption("custom_firmware_version_metadata", "")  # noqa: F821
if metadata:
    version += f".{metadata}" if "+" in version else f"+{metadata}"
if not VERSION_RE.fullmatch(version):
    raise ValueError(f"invalid derived firmware version: {version!r}")

env.Append(  # noqa: F821
    CPPDEFINES=[("PB_FIRMWARE_VERSION", env.StringifyMacro(version))]
)
