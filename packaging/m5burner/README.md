# M5Burner packaging

Build the release firmware and its M5Burner artifacts from the repository root:

```sh
python3 scripts/package_m5burner.py
```

The builder requires Python 3.9 or later.

The builder reads `VERSION`, runs `pio run -e release`, and writes:

```text
dist/pump-bug-<version>.bin
dist/pump-bug-<version>-m5burner.zip
dist/SHA256SUMS
```

The `.bin` is a merged image starting at flash offset `0x0000`, suitable for a
catalog entry that accepts one binary. The ZIP follows M5Stack's
[documented custom-firmware layout](https://docs.m5stack.com/en/related_documents/M5Burner)
and contains `m5burner.json` plus the separate images in `firmware/`, with each
flash offset in its filename.

The StickS3 image layout is fixed by PlatformIO and the project's partition
table:

| Image | Offset |
|---|---:|
| bootloader | `0x0000` |
| partition table | `0x8000` |
| Arduino boot application | `0xe000` |
| Pump Bug application | `0x10000` |

The merged image ends with the application; it does not pad the remainder of
the 8 MB flash with erased bytes. Burning this package with M5Burner erases Pump
Bug settings and shot history. Users must download any shots they want to keep
before reinstalling or updating through M5Burner.

`metadata.json` is the source for the generated `m5burner.json`. Its
`repository` field points at the public GitHub repository. Catalog descriptions,
screenshots, cover artwork, and publisher-account details are submission
metadata rather than part of the firmware artifact, and are not kept here.

For a package-only rerun using an existing release build:

```sh
python3 scripts/package_m5burner.py --skip-build
```

Run the focused packaging test with:

```sh
python3 -m unittest test/test_package_m5burner.py
```

The test verifies component placement, every erased-byte gap, the factory
application partition bound, image signatures, version and package metadata
validation, overlap rejection, and byte-for-byte reproducibility.
