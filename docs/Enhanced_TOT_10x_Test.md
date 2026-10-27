# Optional 10x Dispatch TOT timer test (advanced users only)

This section is **for technically inclined users who want to test the
Enhanced TOT Patch themselves**. It requires editing firmware with a hex
editor. **Everyone else can skip this entire section.**

To make long tests quicker, the timer can run **10 times faster**. For
example, **one minute of key-down time represents ten minutes of normal
timer operation**. A six-minute timeout expires after just 36 seconds.
This helps test the Off setting without spending as much time transmitting.

**WARNING: NEVER leave the 10x timer modification installed for normal operation.**
It makes every programmed Dispatch transmit timeout expire ten times faster:
15 seconds becomes 1.5 seconds, 3 minutes becomes 18 seconds, and 10 minutes
becomes 1 minute. With the Enhanced TOT Patch, `FF (Off)` still disables
Dispatch TOT. This is a diagnostic test, not a new normal timer setting.

First apply the **normal** Enhanced TOT Patch to recognized TK-840 8355 or
9306, or TK-940/941 AB00 firmware and **save that normally patched image** as a separate file. Back it
up before making a test copy. Use a HEX editor on the *test copy only*, and
change BOTH timer bytes from `0A` to `01` using the surrounding bytes as
a cross-check:

| Firmware | CPU address | Offset in firmware-only .bin | Original bytes | TEST bytes |
|---|---|---|---|---|
| TK-840 8355 | `950E` | `150E` | `3A 42 0A 56 09` | `3A 42 01 56 09` |
| TK-840 8355 | `B0E9` | `30E9` | `3A 42 0A 71 7D` | `3A 42 01 71 7D` |
| TK-840 9306 | `9514` | `1514` | `3A 42 0A 56 09` | `3A 42 01 56 09` |
| TK-840 9306 | `B16A` | `316A` | `3A 42 0A 71 7D` | `3A 42 01 71 7D` |
| TK-940/941 AB00 | `950B` | `150B` | `3A 42 0A 56 09` | `3A 42 01 56 09` |
| TK-940/941 AB00 | `B1A5` | `31A5` | `3A 42 0A 71 7D` | `3A 42 01 71 7D` |

**These file offsets are for firmware-only binary files** (CPU address
minus `8000`). They are not offsets in a full backup file or a HEX file.
Change only those two bytes. The toolkit does not generate accelerated
firmware automatically.

When the test image is loaded, the toolkit identifies **10x TOT TEST MODE**
and offers to remove the speed-up in memory (the disk file is never silently
changed). If left enabled, it warns again before a firmware write and offers
**Remove**, **Write TEST firmware**, or **Abort**. This includes development
writes and full restores. Non-interactive writes of 10x test firmware are
refused, even with `--confirm-write`.

**After the experiment, reflash a normal-speed firmware image**, preferably
the saved, normally Enhanced TOT-patched image. Never leave a test copy in
regular radio service.

Return to the [TK-x4x Toolkit README](../README.md).

Copyright (c) 2026 Dave, KY4WI. Licensed under GPL-3.0-or-later.
