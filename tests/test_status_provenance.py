# Copyright (c) 2026 Dave, KY4WI
# SPDX-License-Identifier: GPL-3.0-or-later

"""Regression tests for independent installed-radio and loaded-memory status.

Uses synthetic observations only; no copyrighted firmware or radio is needed.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CORE = (ROOT / "tkx4xtoolkit.c").read_text()

SOURCE = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define main toolkit_original_main
#include "tkx4xtoolkit.c"
#undef main

#define CHECK(expr) do { \
    if (!(expr)) { fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #expr); return 1; } \
} while (0)

static int status_equals(struct app_state *st, int radio, int line, const char *wanted)
{
    char got[160];
    if (radio) format_right_status(st, line, got, sizeof(got));
    else format_left_status(st, line, got, sizeof(got));
    if (strcmp(got, wanted)) {
        fprintf(stderr, "Expected: [%s] got: [%s]\n", wanted, got);
        return 0;
    }
    CHECK(strlen(got) <= (radio ? STATUS_RIGHT_WIDTH : STATUS_LEFT_WIDTH));
    return 1;
}

int main(void)
{
    struct app_state st;
    memset(&st, 0, sizeof(st));

    CHECK(PREVIEW_LINES == 10);
    CHECK(status_equals(&st, 0, 0, "Firmware: Not loaded"));
    CHECK(status_equals(&st, 0, 1, "Source: --"));
    CHECK(status_equals(&st, 0, 5, "Channels: Not loaded"));
    CHECK(status_equals(&st, 1, 0, "Model: Not probed"));
    CHECK(status_equals(&st, 1, 1, "Firmware: Not probed"));

    /* Quick Probe fingerprints the radio but never loads a firmware image. */
    st.connected = 1;
    st.model = MODEL_941;
    strcpy(st.ident, "941");
    st.radio_fingerprinted = 1;
    st.quick_rev = &fw_db[2]; /* AB00 */
    st.radio_tx = TX_FIXED_OFFSET;
    st.radio_dealer_tx = DEALER_TX_FIXED_OFFSET;
    st.radio_tot = TOT_STOCK;
    CHECK(status_equals(&st, 0, 0, "Firmware: Not loaded"));
    CHECK(status_equals(&st, 1, 1, "FW: AB00 (fingerprinted)"));
    CHECK(status_equals(&st, 1, 3, "TX patch: Stock"));
    CHECK(status_equals(&st, 1, 4, "TOT patch: Stock"));

    /* Loading an unrelated image must not imply it is installed. */
    st.firmware_loaded = 1;
    st.firmware_rev = &fw_db[4]; /* TK-840 8355 */
    st.tx = TX_UNKNOWN;
    st.dealer_tx = DEALER_TX_UNKNOWN;
    st.tot = TOT_PATCHED;
    strcpy(st.firmware_source, "tk840_fix.bin");
    strcpy(st.firmware_sha, fw_db[4].full_sha);
    CHECK(status_equals(&st, 0, 0, "Firmware: TK-840 8355"));
    CHECK(status_equals(&st, 0, 1, "Source: tk840_fix.bin"));
    CHECK(status_equals(&st, 0, 2, "Patched: Yes (Enhanced TOT)"));
    CHECK(status_equals(&st, 1, 1, "FW: AB00 (fingerprinted)"));
    CHECK(status_equals(&st, 1, 4, "TOT patch: Stock"));

    /* A subsequent probe changes radio state only. */
    st.radio_tot = TOT_PATCHED;
    CHECK(status_equals(&st, 0, 2, "Patched: Yes (Enhanced TOT)"));
    CHECK(status_equals(&st, 1, 4, "TOT patch: Enhanced"));

    /* A channel file must never be presented as a radio memory jogger. */
    st.channels_loaded = 1;
    strcpy(st.channels_source, "saved_channels.tk4chan");
    st.preview_count = 1;
    st.preview[0].valid = 1;
    st.preview[0].rx_hz = 444125000;
    strcpy(st.preview[0].name, "FILE");
    CHECK(st.radio_preview_count == 0);
    CHECK(status_equals(&st, 0, 5, "Channels: Loaded"));
    CHECK(status_equals(&st, 0, 6, "Channel source: saved_channels.tk4chan"));

    /* Reading real radio channels (or a full backup) authorizes the jogger. */
    radio_preview_from_loaded_channels(&st);
    CHECK(st.radio_preview_count == 1);
    CHECK(!strcmp(st.radio_preview[0].name, "FILE")); /* synthetic source only */
    CHECK(st.radio_preview[0].rx_hz == 444125000);
    forget_radio_firmware_observation(&st);
    st.connected = 0;
    CHECK(status_equals(&st, 1, 1, "Firmware: Not probed"));
    CHECK(status_equals(&st, 0, 0, "Firmware: TK-840 8355"));

    /* A successful full read/write can refresh the installed radio snapshot. */
    remember_loaded_firmware_on_radio(&st);
    st.connected = 1;
    CHECK(st.quick_rev == st.firmware_rev);
    CHECK(st.radio_tot == TOT_PATCHED);
    CHECK(st.radio_full_read);
    CHECK(status_equals(&st, 1, 1, "FW: 8355 (verified)"));

    /* Unsaved patch changes remain explicitly marked as modified. */
    st.firmware_modified = 1;
    strcpy(st.firmware_sha, "0000000000000000000000000000000000000000000000000000000000000000");
    CHECK(status_equals(&st, 0, 1, "Source: tk840_fix.bin (modified)"));
    return 0;
}
"""

class StatusProvenanceTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cc"), "C compiler is unavailable")
    def test_independent_radio_and_loaded_image_status(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "test.c"
            exe = Path(td) / "test"
            src.write_text(SOURCE)
            proc = subprocess.run(
                ["cc", "-std=c11", "-D_DEFAULT_SOURCE", "-D_POSIX_C_SOURCE=200809L",
                 "-O2", "-Wall", "-Wextra", "-Wpedantic",
                 "-I", str(ROOT), str(src), str(ROOT / "fwtransmute.c"),
                 str(ROOT / "firmware_hex.c"), "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            proc = subprocess.run([str(exe)], capture_output=True, text=True,
                                  timeout=15)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_memory_jogger_wired_only_to_radio_operations(self):
        self.assertIn("#define PREVIEW_LINES 10", CORE)
        probe = CORE.split("static int quick_probe(", 1)[1].split("/* ----------------------------- File helpers",1)[0]
        self.assertIn("sample_radio_preview(st, &p, firstcp);", probe)
        fw = CORE.split("static int action_read_firmware_radio(", 1)[1].split("static int action_read_channels_radio(",1)[0]
        self.assertIn("sample_radio_preview(st, &p, NULL);", fw)
        self.assertIn("print_memory_jogger(st);", fw)
        ch = CORE.split("static int action_read_channels_radio(", 1)[1].split("static int save_firmware_hex_to_path(",1)[0]
        self.assertIn("radio_preview_from_loaded_channels(st);", ch)
        self.assertIn("print_memory_jogger(st);", ch)
        backup = CORE.split("static int full_backup(",1)[1].split("/* ------------------------------- UI status",1)[0]
        self.assertIn("print_memory_jogger(st);", backup)
        menu = CORE.split("static int connect_menu(",1)[1].split("static void update_password_state(",1)[0]
        self.assertIn("print_memory_jogger(st);",menu)
        self.assertIn("print_memory_jogger(st);",CORE.split("static int cli_probe_radio(",1)[1].split("static int cli_get_input_file(",1)[0])
        self.assertNotIn("print_memory_jogger(st);",CORE.split("static int load_channels_file(",1)[1].split("enum browse_kind",1)[0])
        self.assertNotIn("print_memory_jogger(st);",CORE.split("static int load_firmware_file(",1)[1].split("static int load_hex_firmware_file(",1)[0])

if __name__ == "__main__":
    unittest.main()
