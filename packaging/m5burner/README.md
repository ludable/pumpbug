# M5Burner packaging

Build the release firmware and its M5Burner package from the repository root:

```sh
python3 scripts/package_m5burner.py
```

The builder requires Python 3.9 or later.

The builder reads `VERSION`, runs `pio run -e release`, and writes:

```text
dist/pump-bug-<version>.bin
dist/SHA256SUMS
```

The `.bin` is a merged image starting at flash offset `0x0000`. M5Burner's web
and macOS publishing tools accept this file as the hardware package.

The StickS3 image layout is fixed by PlatformIO and the project's partition
table:

| Image | Offset |
|---|---:|
| bootloader | `0x0000` |
| partition table | `0x8000` |
| Arduino boot application | `0xe000` |
| Pump Bug application | `0x10000` |

The merged image fills gaps between its components with `0xff`, including the
released NVS range from `0x9000` to `0xe000`. It ends with the application and
does not pad the remainder of the 8 MB flash. The application partition ends at
`0x380000`, where the NVS partition begins. The shot-history partition keeps its
released offset of `0x390000`.

Installing without a full-chip erase preserves shot history when updating from
Pump Bug 0.1.x and resets settings once at the relocated NVS address. Later
updates installed without a full-chip erase preserve both settings and shots.
A full-chip erase deletes all device data.

In the macOS application, choose **Start** without pressing the separate
**Erase** button. In web M5Burner, clear **Erase whole flash before burning
(recommended)** before choosing **Start**.

Test the catalog package on a device before publishing each release. The first
test installation from 0.1.x must preserve shots and reset settings. Configure
the device again, install the same package a second time without a full-chip
erase, and confirm that both settings and shots remain.

For a package-only rerun using an existing release build:

```sh
python3 scripts/package_m5burner.py --skip-build
```

Run the focused packaging test with:

```sh
python3 -m unittest test/test_package_m5burner.py
```

The test verifies component placement, every erased-byte gap, the factory
application partition bound, image signatures, version validation, overlap
rejection, and byte-for-byte reproducibility.
