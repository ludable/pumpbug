# SPDX-FileCopyrightText: 2026 ludable
# SPDX-License-Identifier: AGPL-3.0-only

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile


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
        self.metadata = self.project / "packaging/m5burner/metadata.json"

        self.build.mkdir(parents=True)
        (self.project / "boards").mkdir(parents=True)
        (self.framework / "tools/partitions").mkdir(parents=True)
        self.metadata.parent.mkdir(parents=True)
        (self.project / "VERSION").write_text("0.1.0\n", encoding="ascii")
        (self.project / "boards/m5stick-s3.json").write_text(
            json.dumps({"upload": {"flash_size": "8MB"}}), encoding="utf-8"
        )
        self.metadata_value = {
            "name": "Pump Bug",
            "description": "Test package",
            "keywords": "espresso",
            "author": "Pump Bug",
            "repository": "",
            "firmware_category": {
                "path": "firmware",
                "device": ["M5StickS3"],
                "default_baud": 921600,
            },
            "framework": "Arduino",
        }
        self.write_metadata()

        partition_entry = package_m5burner.PARTITION_ENTRY.pack(
            package_m5burner.PARTITION_MAGIC,
            0x00,
            0x00,
            0x10000,
            0x380000,
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

    def write_metadata(self):
        self.metadata.write_text(json.dumps(self.metadata_value), encoding="utf-8")

    def build_package(self):
        return package_m5burner.build_artifacts(
            self.project,
            self.build,
            self.framework,
            self.metadata,
            self.output,
        )

    def test_builds_reproducible_merged_image_and_zip(self):
        merged_path, zip_path, checksums_path = self.build_package()

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
        self.assertEqual(set(merged[0xE007:0x10000]), {0xFF})

        with zipfile.ZipFile(zip_path) as archive:
            self.assertEqual(
                archive.namelist(),
                [
                    "m5burner.json",
                    "firmware/bootloader_0x0000.bin",
                    "firmware/partitions_0x8000.bin",
                    "firmware/boot_app0_0xe000.bin",
                    "firmware/pump-bug_0x10000.bin",
                ],
            )
            metadata = json.loads(archive.read("m5burner.json"))
            self.assertEqual(metadata["version"], "0.1.0")
            self.assertEqual(
                archive.read("firmware/pump-bug_0x10000.bin"), b"\xe9APP"
            )

        first_hashes = [
            hashlib.sha256(path.read_bytes()).digest()
            for path in (merged_path, zip_path)
        ]
        self.build_package()
        second_hashes = [
            hashlib.sha256(path.read_bytes()).digest()
            for path in (merged_path, zip_path)
        ]
        self.assertEqual(first_hashes, second_hashes)

        checksum_lines = checksums_path.read_text(encoding="ascii").splitlines()
        self.assertEqual(
            checksum_lines,
            [
                f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}"
                for path in (merged_path, zip_path)
            ],
        )

    def test_rejects_overlapping_images(self):
        (self.build / "bootloader.bin").write_bytes(b"\xe9" * (0x8000 + 1))
        with self.assertRaisesRegex(ValueError, "overlaps"):
            self.build_package()

    def test_rejects_application_larger_than_factory_partition(self):
        (self.build / "firmware.bin").write_bytes(b"\xe9" * (0x380000 + 1))
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

    def test_rejects_invalid_metadata(self):
        cases = (
            ("author", None, "must be a string"),
            (
                "firmware_category",
                {
                    "path": "firmware",
                    "device": ["M5StickC"],
                    "default_baud": 921600,
                },
                "must contain only M5StickS3",
            ),
        )
        for field, invalid, message in cases:
            with self.subTest(field=field):
                original = self.metadata_value[field]
                self.metadata_value[field] = invalid
                self.write_metadata()
                with self.assertRaisesRegex(ValueError, message):
                    self.build_package()
                self.metadata_value[field] = original
                self.write_metadata()


if __name__ == "__main__":
    unittest.main()
