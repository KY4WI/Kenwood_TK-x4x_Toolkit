# Copyright (c) 2026 Dave, KY4WI
# SPDX-License-Identifier: GPL-3.0-or-later

"""Verify firmware transmutation UI is present and isolated."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
CORE=(ROOT/"tkx4xtoolkit.c").read_text()
MAKE=(ROOT/"Makefile").read_text()

class TransmutationUITests(unittest.TestCase):
    def test_menu_item(self):
        self.assertIn("""{'T', "Validate / Transmute Firmware", NULL,""",CORE)
        self.assertIn("Validate against known stock firmware, remove recognized patches,",CORE)
        self.assertIn("Changes only the loaded image, not the radio.",CORE)
        self.assertIn("FIRMWARE VALIDATION / TRANSMUTATION",CORE)
        self.assertIn("No firmware loaded. Load firmware now? [n/Y]",CORE)
        self.assertIn("case 't':\n            (void)action_transmute_firmware(st);",CORE)

    def test_patch_toggle_and_verified_restoration(self):
        self.assertIn('"Firmware patch status:\\n"',CORE)
        self.assertIn('"Apply Enhanced TX Patch"',CORE)
        self.assertIn('"Remove Enhanced TX Patch"',CORE)
        self.assertIn('"Apply Enhanced TOT Patch"',CORE)
        self.assertIn('"Remove Enhanced TOT Patch"',CORE)
        self.assertIn('"Remove Legacy TOT Bypass"',CORE)
        self.assertIn("tot_patch_action_label(st->tot)",CORE)
        self.assertNotIn('"Remove TOT Patch"',CORE)
        self.assertIn('if(ctx.supports_tot || (ctx.supports_legacy && st->tot==TOT_LEGACY))',CORE)
        self.assertIn('printf("  TOT Patch:     %s\\n",patch_tot_status(ctx->st))',CORE)
        self.assertIn('if(tot!=TOT_STOCK && tot!=TOT_PATCHED && tot!=TOT_LEGACY)return -1;',CORE)
        self.assertIn('tot_legacy_for_revision(img,rev)',CORE)
        self.assertIn('tot_legacy_region(p,r)',CORE)
        self.assertIn('tot_legacy_region(blocks+offset,r)',CORE)
        self.assertIn('if(!tot_legacy_for_revision(copy,rev))return -1;',CORE)
        self.assertIn("restore_loaded_to_stock(st,stock)",CORE)
        self.assertIn("restore_tk840_stock(st,converted)",CORE)
        self.assertIn("sha256_hex(out,PROGRAM_SIZE,sha)",CORE)
        self.assertIn("fwtransmute_restore_stock", (ROOT/"fwtransmute.c").read_text())
        self.assertIn("fwtransmute_to_8355(st->firmware,PROGRAM_SIZE,converted)",CORE)
        self.assertIn("Transmute loaded 9306 to stock 8355?",CORE)
        self.assertIn('Selected: Transmute TK-840 9306 to stock 8355',CORE)
        self.assertIn('"Remove +25 MHz Offset Patch"',CORE)
        self.assertIn("ctx.has_plus25=st->firmware_rev &&",CORE)
        self.assertIn("tkx4x_plus25_reapply(copy,st->firmware,rev->checksum)",CORE)
        self.assertIn("else if(choice=='o')(void)action_remove_plus25(st);",CORE)
        self.assertIn("Replaced legacy +25 MHz offset patch with Enhanced TX Patch.",CORE)

    def test_tot_status_does_not_depend_on_enhanced_tx(self):
        status = CORE.split("static void format_patch_status(", 1)[1]
        status = status.split("static void format_left_status(", 1)[0]
        self.assertIn('"Patched: Yes (Enhanced TX + Enhanced TOT)"', status)
        self.assertIn('"Patched: Yes (Enhanced TOT)"', status)
        self.assertIn('"Patched: Yes (+25 MHz + Enhanced TOT)"', status)
        self.assertIn('"Patched: Yes (legacy TOT bypass%s)"', status)
        self.assertNotIn('"Patched: Yes (TOT)"', status)

    def test_remove_all_patches_menu(self):
        self.assertIn('"Remove All Patches"',CORE)
        self.assertIn("struct ui_item items[6];",CORE)
        self.assertIn("ctx.has_any_patch=st->firmware_rev &&",CORE)
        self.assertIn("else if(choice=='a')(void)action_remove_all_patches(st);",CORE)
        body=CORE.split("static int action_remove_all_patches(",1)[1]
        body=body.split("static int action_remove_plus25(",1)[0]
        self.assertIn("restore_loaded_to_stock(st,stock)!=0",body)
        self.assertIn("verify_patch_free_stock(stock,rev)!=0",body)
        self.assertIn("memcpy(st->firmware,stock,PROGRAM_SIZE)",body)
        self.assertLess(body.index("verify_patch_free_stock(stock,rev)!=0"),
                        body.index("memcpy(st->firmware,stock,PROGRAM_SIZE)"))

    def test_patch_help_is_always_accessible(self):
        menu=CORE.split("static void firmware_patch_menu(",1)[1]
        menu=menu.split("static void firmware_menu(",1)[0]
        self.assertIn("'I',\"Patch Help / Info\"",menu)
        self.assertIn("firmware_patch_help_screen();",menu)
        self.assertNotIn("if(!st->firmware_loaded){",menu)
        help_text=CORE.split("static void firmware_patch_help_screen(void)",1)[1]
        help_text=help_text.split("static void firmware_patch_menu(",1)[0]
        for message in (
            "HISTORICAL PATCHES", "+25 MHz fixed TX offset",
            "Legacy TOT bypass", "ENHANCED PATCHES", "Enhanced TX",
            "Enhanced TOT", "Adds Off to the normal transmit timeout",
            "Without this patch, that same setting gives a 60-second timeout.",
            "Select Off with CHIRP", "FIRMWARE SUPPORT / TEST STATUS", "hardware-tested",
            "RECOMMENDED: Firmware Menu -> [G] Guided Firmware Workflow",
            "Unknown modifications are refused.",
            "Press any key to return to Firmware Patches."
        ):
            self.assertIn(message,help_text)
        self.assertIn("per_page=rows>7",help_text)
        self.assertIn("fputc('\\n',stdout);",help_text)
        self.assertNotIn("if(g_ansi)",help_text)
        self.assertNotIn("\\033[2J",help_text)
        self.assertNotIn("rows+2u",help_text)
        self.assertNotIn("Stock FF",help_text)
        self.assertNotIn("FF (255)",help_text)

    def test_patch_menu_guides_unsupported_firmware(self):
        self.assertIn("No supported patches for this firmware version.", CORE)
        self.assertIn("upgrade to TK-840 firmware 8355", CORE)
        self.assertIn("TK-840 9306: Enhanced TOT (hardware-tested).", CORE)
        self.assertIn("For Enhanced TX/TOT, use TK-940/941 firmware AB00.", CORE)
        self.assertIn("Transmute supported older revisions, or upgrade firmware.", CORE)
        self.assertIn("Unknown revisions cannot be transmuted automatically.", CORE)
        menu=CORE.split("static void firmware_patch_menu(",1)[1]
        menu=menu.split("static void firmware_menu(",1)[0]
        self.assertIn("st->patch_model=st->firmware_rev ?",menu)
        self.assertIn("ctx.supports_tx=st->firmware_rev &&",menu)
        self.assertIn("ctx.has_any_patch=st->firmware_rev &&",menu)
        self.assertNotIn("patch changes are unavailable.\\n",menu)

    def test_stock_status_and_metadata_cleanup(self):
        self.assertIn('!find_full_revision(st->firmware_sha) ? " (modified)"', CORE)
        self.assertIn("verify_patch_free_stock(copy,rev)<0", CORE)
        self.assertIn("img[MOD_TAG_OFFSET]!=0xff", CORE)
        self.assertIn("memcmp(img+AUTHOR_SIG_OFFSET,AUTHOR_SIG_TEXT,AUTHOR_SIG_LEN)", CORE)
        self.assertIn("!strcmp(sha,rev->full_sha)", CORE)
        self.assertIn("st->firmware_modified=1;", CORE)  # still tracks in-memory edits

    def test_safety_controls(self):
        self.assertIn("fwtransmute_to_ab00(st->firmware,PROGRAM_SIZE,converted,&info)",CORE)
        self.assertIn("if(rc!=FWTRANSMUTE_OK)",CORE)
        self.assertIn("sha256_hex(converted,PROGRAM_SIZE,check)",CORE)
        self.assertIn("memcpy(st->firmware,converted,PROGRAM_SIZE)",CORE)
        self.assertIn("transmute_stock_guidance",CORE)
        self.assertIn("TK-840 8355",CORE)

    def test_remote_mac_upload_includes_transmuter_header(self):
        # fwtransmute.c and tkx4xtoolkit.c both include this header.
        # It must accompany the source files during the remote build.
        self.assertIn("tar cf - $(SRC) fwtransmute.h fwgrimoire_tk840_9306_8355.h",MAKE)

    def test_integrated_transmutation_is_always_built(self):
        self.assertIn("SRC = tkx4xtoolkit.c firmware_hex.c fwtransmute.c", MAKE)
        self.assertNotIn("TKX4X_NO_TRANSMUTER", MAKE)
        self.assertNotIn("TKX4X_NO_TRANSMUTER", CORE)
        self.assertNotIn("FWTRANSMUTE_STANDALONE",
                         (ROOT/"fwtransmute.c").read_text())
        self.assertIn("fwtransmute_to_8355", CORE)
        self.assertIn("fwtransmute_to_ab00", CORE)
        self.assertIn("Try Firmware Menu [T] Validate / Transmute Firmware", CORE)

if __name__=="__main__":
    unittest.main()
