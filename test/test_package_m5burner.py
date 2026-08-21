# SPDX-FileCopyrightText: 2026 ludable
# SPDX-License-Identifier: AGPL-3.0-only

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


PROJECT_ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "package_m5burner", PROJECT_ROOT / "scripts/package_m5burner.py"
)
package_m5burner = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(package_m5burner)


class PackageM5BurnerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.project = self.root / "project"
        self.build = self.project / ".pio/build/release"
        self.framework = self.root / "framework"
        self.output = self.project / "dist"

        self.build.mkdir(parents=True)
        (self.project / "boards").mkdir(parents=True)
        (self.framework / "tools/partitions").mkdir(parents=True)
        (self.project / "VERSION").write_text("0.1.0\n", encoding="ascii")
        (self.project / "boards/m5stick-s3.json").write_text(
            json.dumps({"upload": {"flash_size": "8MB"}}), encoding="utf-8"
        )
        partition_entry = package_m5burner.PARTITION_ENTRY.pack(
            package_m5burner.PARTITION_MAGIC,
            0x00,
            0x00,
            0x10000,
            0x370000,
            b"firmware",
            0,
        )
        partition_table = partition_entry + b"\xff" * 32

        self.parts = {
            "bootloader.bin": b"\xe9BOOT",
            "partitions.bin": partition_table,
            "firmware.bin": b"\xe9APP",
        }
        for name, data in self.parts.items():
            (self.build / name).write_bytes(data)
        (self.framework / "tools/partitions/boot_app0.bin").write_bytes(b"BOOTAPP")

    def tearDown(self):
        self.temp.cleanup()

    def build_package(self):
        return package_m5burner.build_artifacts(
            self.project,
            self.build,
            self.framework,
            self.output,
        )

    def test_builds_reproducible_merged_image(self):
        merged_path, checksums_path = self.build_package()

        merged = merged_path.read_bytes()
        self.assertEqual(merged[0 : len(self.parts["bootloader.bin"])], b"\xe9BOOT")
        partition_end = 0x8000 + len(self.parts["partitions.bin"])
        self.assertEqual(
            merged[0x8000:partition_end], self.parts["partitions.bin"]
        )
        self.assertEqual(merged[0xE000 : 0xE007], b"BOOTAPP")
        self.assertEqual(merged[0x10000 : 0x10004], b"\xe9APP")
        self.assertEqual(set(merged[5:0x8000]), {0xFF})
        self.assertEqual(set(merged[partition_end:0xE000]), {0xFF})
        self.assertEqual(set(merged[0x9000:0xE000]), {0xFF})
        self.assertEqual(set(merged[0xE007:0x10000]), {0xFF})

        first_hash = hashlib.sha256(merged_path.read_bytes()).digest()
        self.build_package()
        second_hash = hashlib.sha256(merged_path.read_bytes()).digest()
        self.assertEqual(first_hash, second_hash)

        checksum_lines = checksums_path.read_text(encoding="ascii").splitlines()
        self.assertEqual(
            checksum_lines,
            [
                f"{hashlib.sha256(merged_path.read_bytes()).hexdigest()}  "
                f"{merged_path.name}"
            ],
        )

    def test_rejects_overlapping_images(self):
        (self.build / "bootloader.bin").write_bytes(b"\xe9" * (0x8000 + 1))
        with self.assertRaisesRegex(ValueError, "overlaps"):
            self.build_package()

    def test_rejects_application_larger_than_factory_partition(self):
        (self.build / "firmware.bin").write_bytes(b"\xe9" * (0x370000 + 1))
        with self.assertRaisesRegex(
            ValueError, "exceeds the factory app partition"
        ):
            self.build_package()

    def test_rejects_invalid_image_signatures(self):
        cases = (
            ("bootloader.bin", b"\x00BOOT", "bootloader.bin is not an ESP image"),
            ("firmware.bin", b"\x00APP", "firmware.bin is not an ESP image"),
            ("partitions.bin", b"\x00" * 64, "not an ESP partition table"),
        )
        for filename, invalid, message in cases:
            with self.subTest(filename=filename):
                path = self.build / filename
                original = path.read_bytes()
                path.write_bytes(invalid)
                with self.assertRaisesRegex(ValueError, message):
                    self.build_package()
                path.write_bytes(original)

    def test_rejects_invalid_version(self):
        (self.project / "VERSION").write_text("release\n", encoding="ascii")
        with self.assertRaisesRegex(ValueError, "invalid firmware version"):
            self.build_package()


if __name__ == "__main__":
    unittest.main()
