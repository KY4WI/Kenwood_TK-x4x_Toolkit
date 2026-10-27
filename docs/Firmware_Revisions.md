# TK-x4x firmware revision references

**Verified** means an original image was analyzed and its SHA-256 recorded.
**Reconstructed and hash-verified** means a user-supplied modified image was
normalized by removing a recognized patch, yielding the exact recorded
stock-image SHA-256. An independent factory file was not required to
identify the firmware revision.
**Reported** means a revision is documented elsewhere without a reference image.

A four-digit Kenwood firmware checksum is a **16-bit additive byte sum**, not
a unique version identifier. Different firmware images can have the same
checksum; modifications usually change it. The toolkit never treats a
reported checksum alone as permission to patch an image.

## TK-840 firmware

| Checksum | Reference | Information |
| --- | --- | --- |
| `8355` | **Verified image** | Latest *standard* release found in historical list; improved LTR missed-call handling. |
| `9306` | **Reconstructed, hash-verified stock** | Prevented data PTT transmission on a busy home channel. A user-supplied HEX image contained a two-byte legacy TOT bypass at CPU B173-B174 (`81 01` replaced by `00 00`). Reversal yields checksum 9306 and the exact recorded stock SHA-256. |
| `6641` | **Reported, no image** | Prevented scan stopping on a deleted system. |
| `9C20` | **Reported, no image** | Mobile-data-specific firmware, dated January 28, 2002; required in Pyramid's TK-840 mobile-data instructions. |

8355 stock image SHA-256:
`7b061dc79ad69475484d68340edd872bd5e1a8f67efeb27bffbe3812a0ee7258`

9306 reconstructed stock SHA-256:
`5eac89945d2a199919ab12b11ca5ffb226452bd3ff1788df898467ba69ab38ce`

The Toolkit recognizes this exact 9306 layout, reports the legacy TOT bypass,
and can remove it via Firmware Patches or Transmute Firmware.
The Firmware Patches menu identifies the installed TOT variant and offers
"Remove Legacy TOT Bypass" or "Remove Enhanced TOT Patch" as appropriate. TK-840 8355 also recognizes
the two-byte historical legacy bypass at B0F2-B0F3, restoring it only when the
entire resulting image matches the verified stock 8355 SHA-256.
A separately sourced factory-original 9306 image is not required for
recognition: the normalized code matches the recorded full-image SHA-256.

8355 appears to be the latest **standard** TK-840 release, but this
does not establish that it was the last firmware of *every type*.
The 9C20 mobile-data variant is separately documented.
**Both 8355 and 9306 have hardware-tested Enhanced TOT support.** The
9306 Enhanced TOT recipe uses the same 48-byte setup routine and the
corresponding 29-byte countdown routine at CPU 94EF and B15F; the original
revision-specific C6F2 target is preserved.
Comparison with the actual original 8355 binary found the TOT setup routine
at CPU 94E9 (8355) / 94EF (9306) to be byte-identical, 48/48 bytes.
The countdown routine at B0DE (8355) / B15F (9306) differs at exactly
three byte positions before normalization: offsets 20-21 are the legacy
bypass (8355 81 01, modified 9306 00 00), and offset 26 is a
revision-specific address byte (8355 39, 9306 F2). Restoring the bypass
leaves only the address byte differing. Byte similarity alone does **not** establish operating behavior; the
9306 hardware test is the separate validation.

The Toolkit's Firmware Menu can **convert TK-840 9306 to verified stock
8355**, including recognized 9306 legacy or Enhanced TOT modifications.
The conversion first normalizes the source to its recorded stock 9306
SHA-256, then executes a source-offset/length recipe and requires an
exact match with the independently verified stock 8355 SHA-256. The recipe
contains no complete firmware image and uses 3,450 copy spans occupying
10,366 bytes of compiled data. It adds no new runtime dependencies.

The Firmware Menu also restores recognized 8355 patches to stock 8355.
Transmutation modifies the loaded image in memory only, after confirmation;
it does not write to the radio. A copy of the original firmware should be
saved before upgrading. The reconstructed 9306 stock image is identified by exact SHA-256,
although no separately sourced factory original was available. The 9306-to-8355
conversion has been successfully hardware-tested on a real TK-840. No TK-840
to TK-940/941 cross-model conversion is supported.

## TK-940 / TK-941 firmware

| Checksum | Reference | Information |
| --- | --- | --- |
| `AB00` | **Verified image** | Standard latest release in historical list. Enhanced TX Patch and Enhanced TOT Patch supported. |
| `B5EC` | **Verified image** | Earlier release, data PTT busy-channel change. Enhanced TX Patch supported. |
| `8E9D` | **Verified image** | Earlier release. Enhanced TX Patch supported. |
| `2CCA` | **Verified image** | Earlier release. Enhanced TX Patch supported. |
| `AAEB` | **Reported, no image** | Listed with improved LTR missed-call handling. |
| `3885` | **Reported, no image** | Specialized mobile-data firmware required by Pyramid Communications. |

The Toolkit's **Firmware Menu → [T] Validate / Transmute Firmware** can
normalize recognized modifications and convert B5EC, 8E9D, or 2CCA to
stock AB00 with full-image SHA-256 verification. Conversion operates on the
loaded firmware image in memory; the radio is unchanged until explicitly written.

## Historical +25 MHz fixed-offset patch

The Firmware Patches menu recognizes the old +25 MHz offset modification
when all three complete instruction sequences match known patterns (three-byte
immediate-only and seven-byte variants, including a mixture). It provides
**Remove +25 MHz Offset Patch** only while that patch is installed. Removal
restores the three complete stock instruction sequences and checks full-image
stock normalizability both before and after.
Other TOT modifications are preserved.

The older +25 MHz modification is superseded by Enhanced TX Patch, which
uses the programmed TX frequency rather than a hard-coded offset. Applying
Enhanced TX automatically removes the old +25 MHz patch first, including
the original seven-byte variant used by `tk941_25.hex`. Applying or removing
Enhanced TOT does not remove the old +25 MHz offset modification. No firmware
images are embedded in tests or the repository.

## Removing all recognized patches

The Firmware Patches menu provides **Remove All Patches** when a recognized
firmware image contains a detected TX, TOT, legacy +25 MHz patch, or project
patch marker. It restores that firmware's original revision in memory, removes
all recognized patch instructions and project metadata, and requires an exact
full-image stock SHA-256 before committing any changes. It does not flash the
radio, switch firmware revisions, or overwrite the input file. Unrecognized
edits fail safely without modifying the loaded image.

## Unrecognized firmware

When a full image is loaded, the toolkit distinguishes:

- **Documented checksum without a reference image:** We are aware of the
  reported revision, but do not have a reference image for analysis.
  A checksum match by itself does not prove that this is that firmware.
- **Known checksum with an unmatched SHA-256 fingerprint:** The image may
  contain modifications or otherwise differ from the verified reference.
- **No matching documented checksum:** The image could be a modified known
  version or a version we have not documented.

A quick probe reads too little to calculate the full image checksum.
Unknown firmware is never patched. Additional revisions may be supported
in future releases if reference images become available.

**IMPORTANT: Kenwood firmware is copyrighted. DO NOT upload Kenwood firmware
files, or links to them, to GitHub, including bug reports and discussions.**

## Sources

- [Historical firmware reference list](https://com-port.blogspot.com/2009/06/kenwood-programming-software-reference.html)
  (third-party; published 2009; TK-840 8355/9306/6641/9C20 and TK-940/941 data).
- [Pyramid Communications TK-x40 mobile-data programming instructions](https://static1.1.sqspcdn.com/static/f/1281716/16754240/1329961995400/40SERIES.pdf)
  (revision C, 2002; distinguishes 9C20, 8355, and 3885).
- [TK-840 8355 checksum and SHA-256 documentation](https://github.com/KY4WI/Kenwood_TK-x4x_Reverse_Engineering/blob/main/tools/legacy/tk840download-README.md).
- The verified firmware database is in `tkx4xtoolkit.c`.

## Diagnostic 512-byte block comparison

For a full firmware read or file load whose whole-image SHA-256 is not an
exact stock match, the Toolkit compares **51 contiguous 512-byte blocks**
spanning CPU addresses `8000-E5FF`. Each reference stores the first
64 bits of each block's SHA-256; the Toolkit contains verified reference
fingerprints for **8355, reconstructed 9306, AB00, 2CCA, 8E9D, and B5EC** without distributing
any Kenwood firmware code.

When a majority of blocks matches one reference, it reports the likely
family, count of identical blocks, and differing CPU address ranges. This
is **diagnostic information only**, not a firmware identity guarantee.
A modified or unknown firmware is **never approved for patching or
transmutation based solely on these fingerprints**. Quick Probe identifies
known firmware from the SHA-256 fingerprint of block `DF00-DF7F`.

Copyright (c) 2026 Dave, KY4WI. Licensed under GPL-3.0-or-later.
