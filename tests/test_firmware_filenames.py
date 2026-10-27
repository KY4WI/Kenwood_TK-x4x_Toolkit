# Copyright (c) 2026 Dave, KY4WI
# SPDX-License-Identifier: GPL-3.0-or-later

"""Regression tests for firmware names, extensions and explicit format selection."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define main original_toolkit_main
#include "tkx4xtoolkit.c"
#undef main

int main(int argc, char **argv)
{
    struct app_state st = {0};
    struct firmware_revision rev = {0};
    char name[MAX_PATH_LEN];
    int mode, rc;
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "name")) {
        rev.model = MODEL_941;
        rev.checksum = 0xAB00;
        st.model = MODEL_941;
        st.firmware_rev = &rev;
        st.firmware_loaded = 1;
        st.tx = TX_FIXED;
        st.dealer_tx = DEALER_TX_FIELD;
        st.tot = TOT_PATCHED;
        proposed_firmware_filename(&st, "bin", name);
        printf("%s\n", name);
        st.tx = TX_FIXED_OFFSET;
        st.dealer_tx = DEALER_TX_FIXED_OFFSET;
        proposed_firmware_filename(&st, "bin", name);
        printf("%s\n", name);
        st.tot = TOT_STOCK;
        proposed_firmware_filename(&st, "bin", name);
        printf("%s\n", name);
        st.firmware_rev = NULL;
        st.model = MODEL_UNKNOWN;
        proposed_firmware_filename(&st, "bin", name);
        printf("%s\n", name);
        return 0;
    }
    if (argc < 4) return 3;
    mode = atoi(argv[3]);
    if (!strcmp(argv[1], "choose")) {
        printf("MODE=%d\n", choose_firmware_file_format(argv[2], mode));
        return 0;
    }
    if (!strcmp(argv[1], "write")) {
        st.model = MODEL_941;
        st.firmware_loaded = 1;
        st.firmware_hex_family = 5;
        rc = write_firmware_to_named_file(&st, argv[2], mode);
        printf("RESULT=%d\n", rc);
        return rc == 0 ? 0 : 4;
    }
    return 5;
}
"""


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class TestFirmwareFileNaming(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        root = Path(cls.tmp.name)
        src = root / "filenames.c"
        cls.exe = root / "filenames"
        src.write_text(SOURCE)
        build = subprocess.run(
            ["cc", "-std=c11", "-O2", "-Wall", "-Wextra",
             "-D_DEFAULT_SOURCE", "-D_POSIX_C_SOURCE=200809L",
             "-I", str(ROOT), str(src), str(ROOT / "firmware_hex.c"),
             str(ROOT / "fwtransmute.c"), "-o", str(cls.exe)],
            capture_output=True, text=True,
        )
        if build.returncode:
            raise AssertionError(build.stderr)

    def run_case(self, command, *args, stdin=""):
        return subprocess.run(
            [str(self.exe), command, *map(str, args)],
            input=stdin, text=True, capture_output=True, timeout=15,
        )

    def test_names_follow_actual_patch_state(self):
        result = self.run_case("name")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), [
            "TK-941_FW_AB00_ETX_ETOT.bin",
            "TK-941_FW_AB00_ETOT.bin",
            "TK-941_FW_AB00_MOD.bin",
            "TK-x4x_FW_0000_UNVERIFIED.bin",
        ])

    def test_typed_extension_is_default(self):
        for path, selected, expected in (
            ("firmware.hex", 0, 1),
            ("firmware.bin", 1, 0),
            ("firmware.HEX", 0, 1),
            ("firmware.BIN", 1, 0),
        ):
            with self.subTest(path=path):
                result = self.run_case("choose", path, selected, stdin="\n")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("MODE=%d" % expected, result.stdout)

    def test_explicit_override_and_unknown_extension(self):
        for path, selected, answer, expected in (
            ("firmware.hex", 0, "b\n", 0),
            ("firmware.bin", 1, "h\n", 1),
            ("firmware.dat", 1, "", 1),
            ("firmware.dat", 0, "", 0),
        ):
            with self.subTest(path=path):
                result = self.run_case("choose", path, selected, stdin=answer)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("MODE=%d" % expected, result.stdout)

    def test_esc_returns_to_filename_editing(self):
        result = self.run_case(
            "choose", "firmware.hex", 0, stdin="\x1b\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("MODE=2", result.stdout)

    def test_writing_mismatched_extension_never_renames(self):
        with tempfile.TemporaryDirectory() as td:
            binary_named_hex = Path(td) / "data.hex"
            a = self.run_case("write", binary_named_hex, 0, stdin="b\n")
            self.assertEqual(a.returncode, 0, a.stdout + a.stderr)
            self.assertEqual(binary_named_hex.stat().st_size, 0x6600)
            self.assertFalse((Path(td) / "data.bin").exists())
            hex_named_binary = Path(td) / "program.bin"
            b = self.run_case("write", hex_named_binary, 1, stdin="h\n")
            self.assertEqual(b.returncode, 0, b.stdout + b.stderr)
            self.assertTrue(hex_named_binary.read_bytes().startswith(b"\r\n\r\n\r\n:"))
            self.assertFalse((Path(td) / "program.hex").exists())

    def test_esc_edit_path_preserves_users_new_name(self):
        with tempfile.TemporaryDirectory() as td:
            old_path = Path(td) / "firmware.hex"
            new_path = Path(td) / "renamed.dat"
            result = self.run_case(
                "write", old_path, 0, stdin="\x1b\n" + str(new_path) + "\n")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(new_path.exists())
            self.assertFalse(old_path.exists())
            self.assertEqual(new_path.stat().st_size, 0x6600)


if __name__ == "__main__":
    unittest.main()
