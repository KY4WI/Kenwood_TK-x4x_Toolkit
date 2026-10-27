# Copyright (c) 2026 Dave, KY4WI
# SPDX-License-Identifier: GPL-3.0-or-later

"""Guided firmware menu and file-browser safety without radio hardware."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "tkx4xtoolkit"
CODE = (ROOT / "tkx4xtoolkit.c").read_text()


class GuidedFirmwareWorkflowTests(unittest.TestCase):
    def test_guided_file_path_does_not_open_radio(self):
        # Main -> Guided -> File -> Abort input format -> Exit.
        proc = subprocess.run(
            [str(EXE), "--no-ansi"],
            input="1\nf\na\n0\n",
            capture_output=True, text=True, timeout=15, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("GUIDED FIRMWARE WORKFLOW", proc.stdout)
        self.assertIn("Read firmware from radio", proc.stdout)
        self.assertIn("Read firmware from file", proc.stdout)
        self.assertEqual(proc.stdout.count(
            "Prepare, then save to file or write to radio."), 2)
        self.assertIn("Selected: Read firmware from file", proc.stdout)
        self.assertIn("Select a firmware file format:", proc.stdout)
        self.assertNotIn("Serial port:", proc.stdout)
        self.assertNotIn("Connection failed", proc.stdout)

    def test_guided_direct_main_menu_shortcut_and_firmware_menu(self):
        for keys in ("g\nf\na\n0\n", "4\ng\nf\na\nb\n0\n"):
            with self.subTest(keys=keys):
                proc = subprocess.run(
                    [str(EXE), "--no-ansi"], input=keys,
                    capture_output=True, text=True, timeout=15, check=False,
                )
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertIn("GUIDED FIRMWARE WORKFLOW", proc.stdout)
                self.assertIn("Selected: Read firmware from file", proc.stdout)
                self.assertNotIn("Connection failed", proc.stdout)

    def test_guided_cli_launches_same_workflow_without_main_menu(self):
        # The one-word command is a shortcut, not another patch engine.
        for args in (["guided", "--no-ansi"], ["--no-ansi", "guided"]):
            with self.subTest(args=args):
                proc = subprocess.run(
                    [str(EXE), *args], input=chr(10).join(["f", "a", ""]),
                    capture_output=True, text=True, timeout=15,
                )
                self.assertIn("GUIDED FIRMWARE WORKFLOW", proc.stdout)
                self.assertIn("Select a firmware file format:", proc.stdout)
                self.assertIn("Selected: Read firmware from file", proc.stdout)
                self.assertNotIn("MAIN MENU", proc.stdout)
                self.assertNotIn("Connection failed", proc.stdout)

    def test_guided_cli_rejects_ignored_options(self):
        for options in (
            ["--input", "unused.bin"],
            ["--output", "unused.bin"],
            ["--no-sparse"],
            ["--patches", "tot"],
            ["--omit-chirp-tag"],
        ):
            with self.subTest(options=options):
                proc = subprocess.run(
                    [str(EXE), "guided", *options],
                    capture_output=True, text=True, timeout=15,
                )
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn(
                    "guided selects source, output and patches interactively",
                    proc.stderr)
                self.assertNotIn("GUIDED FIRMWARE WORKFLOW", proc.stdout)

    def test_guided_cli_is_documented(self):
        proc = subprocess.run(
            [str(EXE), "--help"],
            capture_output=True, text=True, timeout=15,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("guided               Launch the interactive Guided Firmware Workflow.", proc.stdout)
        self.assertNotIn("auto-patch-radio", proc.stdout)

    def test_main_menu_removes_legacy_auto_patch_entry(self):
        proc = subprocess.run(
            [str(EXE), "--no-ansi"], input="0\n",
            capture_output=True, text=True, timeout=15, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertNotIn("3. Auto [P]atch Radio", proc.stdout)
        self.assertIn("1. [G]uided Firmware Workflow", proc.stdout)
        self.assertIn("4. [F]irmware Menu", proc.stdout)
        self.assertIn("6. Full [B]ackup", proc.stdout)
        self.assertIn("8. [A]dvanced / Special Options", proc.stdout)

    def test_obsolete_beta_auto_patch_cli_removed(self):
        help_result = subprocess.run(
            [str(EXE), "--help"], capture_output=True, text=True, timeout=15,
        )
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        self.assertNotIn("auto-patch-radio", help_result.stdout)
        run = subprocess.run(
            [str(EXE), "auto-patch-radio"],
            capture_output=True, text=True, timeout=15,
        )
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("unknown command: auto-patch-radio", run.stderr)
        # The still-supported explicit firmware-patch CLI must never
        # dispatch into the removed sparse Auto Patch implementation.
        cli = CODE.split("static int cli_firmware_patch(", 1)[1]
        cli = cli.split("static int cli_firmware_loader_write(", 1)[0]
        self.assertIn("action_read_firmware_radio(st)", cli)
        self.assertIn("action_patch_loaded(st)", cli)
        self.assertIn("action_write_firmware_radio(st)", cli)
        self.assertNotIn("cli_run_auto_patch", CODE)
        self.assertNotIn("struct sparse_fw_cache", CODE)

    def test_guided_radio_and_file_safety_order(self):
        workflow = CODE.split("static int action_guided_firmware(", 1)[1]
        workflow = workflow.split("/* ------------------------------- Submenus", 1)[0]
        radio = workflow.index("action_read_firmware_radio(st)")
        prep = workflow.index("guided_prepare_firmware(st)")
        no_change = workflow.index("if (from_radio && !prepared_modified)")
        radio_backup = workflow.index("guided_backup_radio_original(st, original")
        self.assertLess(radio, prep)
        self.assertLess(prep, no_change)
        self.assertLess(no_change, radio_backup)
        # If the firmware is already at the recommended revision with its
        # patches, the guided flow must not offer a write or prompt for backup.
        self.assertIn("already_current = 1;", workflow)
        self.assertIn("if (!written && !already_current)", workflow)
        self.assertIn("|| written || already_current) continue;", workflow)
        self.assertIn("} else if (from_radio &&", workflow)

        # The file-source path must read the radio itself, rather than
        # backing up the loaded file, and skip backup when no write is needed.
        file_read = workflow.index("guided_read_radio_original(st, prewrite")
        compare = workflow.index("if (!memcmp(current, st->firmware, PROGRAM_SIZE))")
        file_backup = workflow.index("guided_backup_radio_original(st, prewrite")
        write = workflow.index("action_write_firmware_radio_mode(st, 0, current, original_id)")
        self.assertLess(file_read, compare)
        self.assertLess(compare, file_backup)
        self.assertLess(file_backup, write)
        self.assertIn("if (!from_radio &&", workflow)
        self.assertIn("guided_target_compatible(st)", workflow)
        self.assertIn("const uint8_t *current = from_radio ? original : prewrite;", workflow)
        self.assertIn("st->firmware_modified = prepared_modified", workflow)
        self.assertNotIn("memcmp(prewrite, original, PROGRAM_SIZE)", workflow)

        # Reuse the existing full snapshot for sparse page comparisons; still
        # re-identify the connected target when entering the write session.
        normal_write = CODE.split("static int program_firmware_normal(", 1)[1]
        normal_write = normal_write.split("static int full_firmware_recovery(", 1)[0]
        self.assertIn("if (known_current)", normal_write)
        self.assertIn("if (expected_ident && strcmp(ident, expected_ident))", normal_write)

    @unittest.skipUnless(shutil.which("cc"), "C compiler is unavailable")
    def test_binary_firmware_browser_filters_exact_sizes_not_extensions(self):
        source = r"""
#include <stdint.h>
#include <stdio.h>
#define main toolkit_original_main
#include "tkx4xtoolkit.c"
#undef main
int main(void) {
    if (PROGRAM_SIZE != 0x6600 || FULL_SIZE != 0x7e00) return 1;
    if (!browse_file_allowed(BROWSE_FIRMWARE, "radio.dat", PROGRAM_SIZE)) return 2;
    if (!browse_file_allowed(BROWSE_FIRMWARE, "backed_up", FULL_SIZE)) return 3;
    if (browse_file_allowed(BROWSE_FIRMWARE, "image.bin", PROGRAM_SIZE - 1)) return 4;
    if (browse_file_allowed(BROWSE_FIRMWARE, "image.bin", FULL_SIZE + 1)) return 5;
    if (!browse_file_allowed(BROWSE_HEX, "firmware.hex", 12)) return 6;
    if (browse_file_allowed(BROWSE_HEX, "firmware.bin", PROGRAM_SIZE)) return 7;
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as td:
            file = Path(td) / "check.c"
            exe = Path(td) / "check"
            file.write_text(source)
            build = subprocess.run(
                ["cc", "-std=c11", "-D_DEFAULT_SOURCE",
                 "-D_POSIX_C_SOURCE=200809L", "-O2",
                 "-I", str(ROOT), str(file),
                 str(ROOT / "fwtransmute.c"),
                 str(ROOT / "firmware_hex.c"), "-o", str(exe)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(exe)], capture_output=True,
                                    text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
