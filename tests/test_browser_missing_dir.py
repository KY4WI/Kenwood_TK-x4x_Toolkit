# Copyright (c) 2026 Dave, KY4WI
# SPDX-License-Identifier: GPL-3.0-or-later

"""File-browser recovery regression tests (Linux CI, no radio required)."""

import os
import pathlib
import subprocess
import tempfile
import unittest


EXECUTABLE = pathlib.Path(__file__).resolve().parents[1] / "tkx4xtoolkit"
# In non-TTY mode the UI takes one choice per input line.
BROWSE_THEN_EXIT = "4\nf\nb\nb\n0\n"


class TestDeletedDirectoryRecovery(unittest.TestCase):
    def test_deleted_working_directory_before_launch(self):
        with tempfile.TemporaryDirectory() as parent:
            deleted = pathlib.Path(parent) / "removed"
            deleted.mkdir()
            saved_cwd = os.getcwd()
            try:
                os.chdir(deleted)
                deleted.rmdir()
                result = subprocess.run(
                    [str(EXECUTABLE), "--no-ansi"],
                    input=BROWSE_THEN_EXIT,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    timeout=15,
                    check=False,
                )
            finally:
                os.chdir(saved_cwd)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("Directory: ", result.stdout)
            self.assertIn("Select firmware file", result.stdout)
            self.assertNotIn("Unable to read directory", result.stdout)

    def test_selected_directory_removed_after_startup(self):
        with tempfile.TemporaryDirectory() as parent:
            deleted = pathlib.Path(parent) / "selected"
            deleted.mkdir()
            process = subprocess.Popen(
                [str(EXECUTABLE), "--no-ansi"],
                cwd=deleted,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
            try:
                # MAIN MENU means startup has saved the original CWD in
                # app_state.file_dir, before the directory is deleted.
                banner = []
                for _ in range(80):
                    line = process.stdout.readline()
                    if not line:
                        break
                    banner.append(line)
                    if "MAIN MENU" in line:
                        break
                self.assertIn("MAIN MENU", "".join(banner))
                deleted.rmdir()
                tail, _ = process.communicate(BROWSE_THEN_EXIT, timeout=15)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)
            output = "".join(banner) + tail
            self.assertEqual(process.returncode, 0, output)
            self.assertIn("Directory no longer accessible:", output)
            self.assertIn("Continuing in:", output)
            self.assertIn("Select firmware file", output)


if __name__ == "__main__":
    unittest.main()
