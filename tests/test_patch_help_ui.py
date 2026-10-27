# Copyright (c) 2026 Dave, KY4WI
# SPDX-License-Identifier: GPL-3.0-or-later

"""Exercise patch Help / Info without a loaded firmware image.

The test uses the normal non-TTY plaintext UI and no Kenwood firmware data.
"""
from pathlib import Path
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[1]
EXE=ROOT/"tkx4xtoolkit"

class PatchHelpUITests(unittest.TestCase):
    def test_info_available_before_loading_firmware(self):
        result=subprocess.run(
            [str(EXE),"--no-ansi"],
            input="4\np\ni\n\n\nb\nb\n0\n",
            capture_output=True,text=True,timeout=20,check=False)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        output=result.stdout
        self.assertIn("[I] Patch Help / Info",output)
        self.assertIn("No firmware loaded. Read or load firmware",output)
        self.assertIn("Selected: Patch Help / Info",output)
        for expected in (
            "HISTORICAL PATCHES", "+25 MHz fixed TX offset",
            "Legacy TOT bypass", "ENHANCED PATCHES",
            "Talk Around then controls the TA icon",
            "Adds Off to the normal transmit timeout settings.",
            "Without this patch, that same setting gives a 60-second timeout.",
            "TK-840 9306: Enhanced TOT (hardware-tested).",
            "RECOMMENDED: Firmware Menu -> [G] Guided Firmware Workflow",
            "For manual control, use [T] Validate / Transmute and [P] Patches.",
            "Nothing is flashed until you explicitly choose Write to Radio."
        ):
            self.assertIn(expected,output)
        self.assertNotIn("\\x1b[2J",output)
        self.assertNotIn("Stock FF",output)
        self.assertNotIn("FF (255)",output)
        self.assertIn("Press any key for more",output)
        self.assertIn("Press any key to return to Firmware Patches.",output)

if __name__=="__main__":
    unittest.main()
