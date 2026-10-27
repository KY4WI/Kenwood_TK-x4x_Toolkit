# Copyright (c) 2026 Dave, KY4WI
# SPDX-License-Identifier: GPL-3.0-or-later

"""Reported firmware checksums are informational and never patch whitelists."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
SRC=(ROOT/"tkx4xtoolkit.c").read_text()
DOC=(ROOT/"docs"/"Firmware_Revisions.md").read_text()

class FirmwareReferenceTests(unittest.TestCase):
    def test_reported_versions_are_not_supported_fingerprints(self):
        known=SRC.split("static const struct firmware_revision fw_db[] = {",1)[1].split("#define FW_DB_COUNT",1)[0]
        reported=SRC.split("static const struct reported_fw_checksum reported_fw[] = {",1)[1].split("};",1)[0]
        for code in ("6641","9C20","AAEB","3885"):
            self.assertIn("0x"+code,reported)
            self.assertNotIn("0x"+code,known)
        self.assertIn('"TK-840 9306", MODEL_840, 0x9306',known)
        self.assertNotIn("0x9306",reported)

    def test_near_exact_modified_firmware_message(self):
        comparison=SRC.split("static void print_firmware_similarity(",1)[1]
        comparison=comparison.split(
            "static const struct firmware_revision *find_quick_revision(",1)[0]
        self.assertIn("best_count >= FW_FINGERPRINT_COUNT - 1u",comparison)
        self.assertIn("Firmware appears to be a modified %s image.",comparison)
        self.assertIn("Stock block match: %zu/%zu (512 bytes per block).",comparison)
        self.assertIn("Blocks differing from stock (CPU address ranges):",comparison)
        self.assertIn("These are block boundaries, not the exact modified addresses.",comparison)
        self.assertIn(
            "Block similarity alone does not verify a patch or conversion.",comparison)
        self.assertNotIn("Similarity NEVER authorizes",comparison)

    def test_cautious_user_messages(self):
        self.assertIn("A matching checksum does not prove identity.",SRC)
        self.assertIn("modified version of known firmware",SRC)
        self.assertIn("A copy of this firmware would allow us to investigate adding",SRC)
        self.assertIn("A reference copy would help us identify this firmware",SRC)
        self.assertIn("DO NOT upload Kenwood",SRC)
        self.assertNotIn("sharing it with the project",SRC)

    def test_documented_versions(self):
        for code in ("8355","9306","6641","9C20","AB00","B5EC","8E9D","2CCA","AAEB","3885"):
            self.assertIn(code,DOC)
        self.assertIn("**Verified**",DOC)
        self.assertIn("**Reported**",DOC)

if __name__=="__main__":
    unittest.main()
