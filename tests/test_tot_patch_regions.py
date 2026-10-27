# Copyright (c) 2026 Dave, KY4WI
# SPDX-License-Identifier: GPL-3.0-or-later

"""Check the exact AB00 and 8355 TOT patch regions without firmware images.

AB00 was verified in the emulator. TK-840 8355 was also hardware-tested
with a 10x accelerated timer and sustained transmission.
"""
import hashlib
from pathlib import Path
import re
import unittest


SOURCE = (Path(__file__).resolve().parents[1] / "tkx4xtoolkit.c").read_text()

AB00_EXPECTED = {
    0x94E6: (
        48,
        "92448A018D76E402EB48ED9CE28A9B7C6790CC7075AC38BBED983ED64F302824",
        "41D947FA8AA084291B8C0A97D18B7B0D4EBE751F241718FA511F5DDE12E62792",
    ),
    0xB19A: (
        29,
        "2A1F38E16141CBC21A038AB94E4A949D83C311E2CDDBF6E08D1D33D654BA24A1",
        "08ACBCA008A55D2BD160B17D2907D335F12E6BFF5A4BBA83CF258A7ED57C6B97",
    ),
    0xD6F2: (
        40,
        "677DECB274C51AD72A473588CF6804EA9102A66E6B567CCC3124924115215408",
        "4EED51FE8497E7CE362EEB32B54A64ADAA0D19772FC59B04C51461BBC566E598",
    ),
}


TK840_EXPECTED = {
    0x94E9: (
        48,
        "92448A018D76E402EB48ED9CE28A9B7C6790CC7075AC38BBED983ED64F302824",
        "41D947FA8AA084291B8C0A97D18B7B0D4EBE751F241718FA511F5DDE12E62792",
    ),
    0xB0DE: (
        29,
        "E7F19B7CBE7A117AD020D5FDF7C2AF1CDDD9E859B508B4D300FF7F3415D374DA",
        "9171A338D33F95E70533E48801DA6D739E259AF958E90FD1370ED67447A2097F",
    ),
}


def patch_regions(name, count):
    table = SOURCE.split(
        f"static const struct tot_patch_region {name}[{count}] = {{", 1
    )[1]
    table = table.split("\n};", 1)[0]
    records = re.findall(
        r"\{0x([0-9A-Fa-f]{4})u,\s*(\d+)u,\s*\{([^}]*)\},\s*\{([^}]*)\}(?:,\s*1u,\s*26u,\s*27u)?\}",
        table,
        flags=re.DOTALL,
    )
    parsed = {}
    for address, length, original, modified in records:
        def raw(chunk):
            return bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", chunk))
        parsed[int(address, 16)] = int(length), raw(original), raw(modified)
    return parsed


class TestTOTPatch(unittest.TestCase):
    def assert_reference(self, name, count, reference, expected_delta):
        regions = patch_regions(name, count)
        self.assertEqual(set(regions), set(reference))
        total_delta = 0
        for address, (length, old, new) in regions.items():
            self.assertEqual(len(old), length, hex(address))
            self.assertEqual(len(new), length, hex(address))
            expected_length, old_sha, new_sha = reference[address]
            self.assertEqual(length, expected_length)
            self.assertEqual(hashlib.sha256(old).hexdigest().upper(), old_sha)
            self.assertEqual(hashlib.sha256(new).hexdigest().upper(), new_sha)
            total_delta += sum(new) - sum(old)
        self.assertEqual(total_delta, expected_delta)

    def test_ab00_emulated_bytes(self):
        self.assert_reference(
            "ab00_tot_regions", 3, AB00_EXPECTED, 0x033F)

    def test_8355_hardware_tested_bytes(self):
        self.assert_reference(
            "tk840_tot_regions", 2, TK840_EXPECTED, 0x04C6)

    def test_legacy_tot_bypass_uses_one_known_region_per_revision(self):
        for name, count in (("ab00_tot_regions", 3), ("tk840_tot_regions", 2)):
            regions = patch_regions(name, count)
            address, (size, original, modified) = sorted(regions.items())[1]
            self.assertEqual(original[20:22], bytes.fromhex("8101"), hex(address))
            legacy = bytearray(original)
            legacy[20:22] = bytes.fromhex("0000")
            self.assertEqual(len(legacy), size)
            self.assertEqual(
                [i for i in range(size) if legacy[i] != original[i]],
                [20, 21],
            )
            legacy[20:22] = original[20:22]
            self.assertEqual(bytes(legacy), original)
        self.assertIn('tot_legacy_for_revision(img,rev)', SOURCE)
        self.assertIn('tot_legacy_region(p,r)', SOURCE)
        self.assertIn('tot_legacy_region(blocks+offset,r)', SOURCE)
        self.assertIn('tot==TOT_PATCHED || tot==TOT_LEGACY', SOURCE)

    def test_all_or_none_guard_for_each_revision(self):
        # Reject mixed stock/patched regions without altering the image.
        self.assertIn("if(stock!=count && patched!=count)return TOT_CONFLICT;", SOURCE)
        self.assertIn("return stock==count?TOT_STOCK:TOT_PATCHED;", SOURCE)
        self.assertIn("if(state!=TOT_STOCK)", SOURCE)
        self.assertIn("static enum tot_behavior tot_classify_for_revision(", SOURCE)
        self.assertIn("if(!rev)return TOT_UNSUPPORTED;", SOURCE)
        self.assertNotIn("for(off=0;off+regions[0].length<=PROGRAM_SIZE;off++)", SOURCE)
        self.assertIn("rev->checksum==0x8355", SOURCE)
        self.assertIn("rev->checksum==0xAB00", SOURCE)

    def test_metadata_decoding_keeps_tx_and_tot_bits_distinct(self):
        self.assertIn("#define MOD_TAG_PREFIX          0x80u", SOURCE)
        self.assertIn("#define MOD_TAG_COMPAT_PREFIX   0x40u", SOURCE)
        self.assertIn("#define MOD_TAG_TOT             0x01u", SOURCE)
        self.assertRegex(SOURCE, r"#define MOD_TAG_TX\s+0x02u")
        self.assertIn("if((old&0x03u)==0x03u)result|=MOD_TAG_TX;", SOURCE)
        self.assertIn("if(old&0x08u)result|=MOD_TAG_TOT;", SOURCE)
        self.assertIn("classify_tot_firmware_for_model(img,model)==TOT_PATCHED", SOURCE)


if __name__ == "__main__":
    unittest.main()
