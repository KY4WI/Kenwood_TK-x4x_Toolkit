# Copyright (c) 2026 Dave, KY4WI
# SPDX-License-Identifier: GPL-3.0-or-later

"""Menu display regression: setting names and values must remain readable."""

import pathlib
import subprocess
import unittest


EXECUTABLE = pathlib.Path(__file__).resolve().parents[1] / "tkx4xtoolkit"


class TestMenuLayout(unittest.TestCase):
    def test_advanced_menu_keeps_short_states_and_wraps_long_paths(self):
        # In non-TTY mode, the Toolkit reads one menu command per line.
        # A long backup path must not consume the label column or vanish.
        path = "/tmp/removed-location/" + ("A" * 45) + "/backups"
        result = subprocess.run(
            [str(EXECUTABLE), "--no-ansi", "--backup-dir", path],
            input="8\nb\n0\n",
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=15,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("[D] Backup directory", result.stdout)
        self.assertIn("[W] Firmware write mode  (sparse write)", result.stdout)
        self.assertIn("Value:", result.stdout)
        self.assertIn("/tmp/removed-location/", result.stdout)
        self.assertIn("AAAAA", result.stdout)

    def test_shared_status_and_menu_separator(self):
        source = (EXECUTABLE.parent / "tkx4xtoolkit.c").read_text()
        self.assertIn("#define UI_MENU_LEFT_WIDTH STATUS_LEFT_WIDTH", source)
        self.assertIn("#define UI_MENU_RIGHT_WIDTH STATUS_RIGHT_WIDTH", source)
        self.assertIn("shortcut_used", source)
        # Split-menu rows have a two-character selection prefix and a
        # padded label. Their separator must land on the status separator.
        self.assertIn('UI_MENU_LEFT_WIDTH - 2, UI_MENU_LEFT_WIDTH - 2, text',source)
        self.assertIn('printf(" | %-*.*s", UI_MENU_RIGHT_WIDTH',source)

        output = subprocess.run(
            [str(EXECUTABLE), "--no-ansi"],
            input="8\nb\n0\n",
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=15,
            check=False,
        )
        self.assertEqual(output.returncode, 0, output.stdout)
        rows = output.stdout.splitlines()
        status = [line for line in rows
                  if "LOADED IN MEMORY" in line and "CONNECTED RADIO" in line]
        menu = [line for line in rows
                if "[D] Backup directory" in line and " | " in line]
        self.assertTrue(status, output.stdout)
        self.assertTrue(menu, output.stdout)
        self.assertEqual(status[0].index("|"), menu[0].index("|"))

    def test_status_shows_radio_and_memory_instead_of_channels(self):
        result = subprocess.run(
            [str(EXECUTABLE), "--no-ansi"],
            input="0\n",
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=15,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("LOADED IN MEMORY", result.stdout)
        self.assertIn("CONNECTED RADIO", result.stdout)
        self.assertIn("Firmware: Not loaded", result.stdout)
        self.assertIn("Channels: Not loaded", result.stdout)
        self.assertNotIn("No usable channel preview", result.stdout)

    def test_default_menu_names_not_clipped(self):
        result = subprocess.run(
            [str(EXECUTABLE), "--no-ansi"],
            input="0\n",
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=15,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("8. [A]dvanced / Special Options", result.stdout)
        self.assertIn("1. [G]uided Firmware Workflow", result.stdout)
        self.assertIn("2. [S]et Programmer Port", result.stdout)
        self.assertIn("0. [E]xit", result.stdout)

    def test_firmware_menu_distinguishes_binary_and_hex(self):
        result = subprocess.run(
            [str(EXECUTABLE), "--no-ansi"],
            input="4\nb\n0\n",
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=15,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("Read from BIN File", result.stdout)
        self.assertIn("Read from HEX File", result.stdout)
        self.assertIn("Write to BIN File", result.stdout)
        self.assertIn("Write to HEX File", result.stdout)
        self.assertIn("Write to Radio", result.stdout)
        self.assertNotIn("OPTION INFO", result.stdout)
        self.assertIn("FIRMWARE MENU", result.stdout)
        self.assertIn("| INFO", result.stdout)
        self.assertNotIn("Loader: ", result.stdout)


if __name__ == "__main__":
    unittest.main()
