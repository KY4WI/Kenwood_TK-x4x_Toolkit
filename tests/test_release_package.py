# Copyright (c) 2026 Dave, KY4WI
# SPDX-License-Identifier: GPL-3.0-or-later

"""Check platform-specific releases without requiring cross-compilers or a Mac."""
import hashlib
import os
import shutil
import subprocess
import pathlib
import re
import sys
import tarfile
import zipfile
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from package_release import (ARCHIVE_ROOT, ARCHIVE_LABELS, BINARIES,
                             release_executable_name, markdown_to_text, package)


class ReleasePackageTests(unittest.TestCase):
    def test_readme_conversion(self):
        example = """# Project **Name**

## Programming cable

Use a USB-to-TTL adapter. A wiring diagram is provided below.

<img src="DIY_KPG-4_Programming_Interface.png" alt="wiring" width="500">

Read the [CHIRP driver](https://example.org/chirp) and `README.md`.

```sh
make dist
```
"""
        rendered = markdown_to_text(example)
        self.assertIn("Project Name\n============", rendered)
        self.assertIn(
            "Programming cable diagram:\n"
            "https://github.com/KY4WI/Kenwood_TK-x4x_Toolkit/blob/main/"
            "DIY_KPG-4_Programming_Interface.png", rendered)
        self.assertIn("CHIRP driver (https://example.org/chirp)", rendered)
        self.assertIn("README.md", rendered)
        self.assertIn("Use a USB-to-TTL adapter.", rendered)
        self.assertNotIn("A wiring diagram is provided below.", rendered)
        self.assertIn("    make dist", rendered)
        self.assertNotIn("<img", rendered)
        self.assertNotIn("```", rendered)
        self.assertNotIn("**", rendered)

    def test_numbered_list_continuation_alignment(self):
        example = (
            "## Getting started\n\n"
            "2. Select [Q]uick Probe Radio. If no programmer port is selected,\n"
            "   the Toolkit will prompt you to select or detect your USB adapter.\n"
            "3. Select Full Backup to save the original firmware and channel\n"
            "   data. Keep your backup safe before making changes.\n"
            "4. Use the menus as needed. Firmware updates are optional,\n"
            "   not a required step.\n"
        )
        output = markdown_to_text(example)
        self.assertEqual(
            output.splitlines()[3:8],
            [
                "2. Select [Q]uick Probe Radio. If no programmer port is selected, the Toolkit",
                "   will prompt you to select or detect your USB adapter.",
                "3. Select Full Backup to save the original firmware and channel data. Keep your",
                "   backup safe before making changes.",
                "4. Use the menus as needed. Firmware updates are optional, not a required step.",
            ],
        )

    def test_wraps_paragraphs_lists_and_tables(self):
        example = (
            "# Readme\n\n"
            + "A very long paragraph with ordinary words that should be wrapped "
              "at the requested width without losing any information. " * 3
            + "\n\n"
            + "- Long list item with a detailed explanation that keeps going "
              "well beyond the target width without losing its bullet marker. " * 3
            + "\n\n"
            + "| Firmware | CPU address | Offset in firmware-only .bin "
              "| Original bytes | TEST bytes |\n"
            + "|---|---|---|---|---|\n"
            + "| TK-940/941 AB00 | 950B | 150B | 3A 42 0A 56 09 "
              "| 3A 42 01 56 09 |\n"
            + "\n```sh\n" + "a" * 96 + "\n```\n"
        )
        converted = markdown_to_text(example)
        self.assertIn("    " + "a" * 96, converted)
        self.assertIn("  Firmware: TK-940/941 AB00", converted)
        self.assertIn("  TEST bytes: 3A 42 01 56 09", converted)
        self.assertTrue(any(line.startswith("  ") and
                            "target width" in line
                            for line in converted.splitlines()))
        for line in converted.splitlines():
            if not line.startswith("    " + "a" * 96):
                self.assertLessEqual(len(line), 80, line)

    def test_release_readme_wraps_real_prose(self):
        source = (ROOT / "README.md").read_text(encoding="utf-8")
        text = markdown_to_text(source)
        self.assertIn("Programming cable diagram:", text)
        self.assertIn("KY4WI FW MODS", text)
        # Code examples and URLs stay intact; ordinary prose and table records
        # are limited to the release README's 80-column convention.
        for line in text.splitlines():
            if len(line) > 80:
                self.assertTrue(
                    "http://" in line or "https://" in line or
                    line.startswith("    "),
                    "Unexpected long line: " + line,
                )

    def test_release_version_consistency(self):
        source = (ROOT / "tkx4xtoolkit.c").read_text(encoding="utf-8")
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        building = (ROOT / "BUILDING.md").read_text(encoding="utf-8")
        match = re.search(r'^#define TOOL_VERSION "([^"]+)"$', source, re.M)
        self.assertIsNotNone(match)
        version = match.group(1)
        self.assertRegex(
            version, r"^v[0-9]+\.[0-9]+\.[0-9]+(?:-(?:alpha|beta|rc)\.[0-9]+)?$")
        self.assertIn(" * Version: " + version, source)
        self.assertIn("Current version: **" + version + "**", readme)
        self.assertIn("Development builds show `" + version + " (git", building)
        for label in ARCHIVE_LABELS:
            suffix = ".zip" if label.startswith("windows-") else ".tar.gz"
            self.assertIn(
                "tkx4xtoolkit-" + version + "-" + label + suffix, building)

    def test_archives_are_separate_and_complete(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            dist = root / "dist"
            dist.mkdir()
            for name in BINARIES:
                (dist / name).write_bytes(("fake build: " + name).encode())
            (root / "README.md").write_text(
                "# Toolkit\n\n[Project](https://example.org)\n", encoding="utf-8")
            (root / "LICENSE").write_text("GPLv3 test fixture\n", encoding="utf-8")
            archives = package(dist, "v1.0.0-beta.1", root)
            self.assertEqual(len(archives), 7)
            self.assertEqual({a.parent for a in archives}, {dist / "releases"})
            self.assertEqual(set((dist / "releases").iterdir()), set(archives))
            self.assertFalse((dist / "tkx4xtoolkit-v1.0.0-beta.1.tar.gz").exists())
            for name, label, archive_path in zip(BINARIES, ARCHIVE_LABELS, archives):
                packed_name = release_executable_name("v1.0.0-beta.1", label)
                is_windows = label.startswith("windows-")
                suffix = ".zip" if is_windows else ".tar.gz"
                self.assertEqual(
                    archive_path.name, "tkx4xtoolkit-v1.0.0-beta.1-" + label + suffix)
                expected = {
                    ARCHIVE_ROOT + "/" + packed_name,
                    ARCHIVE_ROOT + "/README.txt",
                    ARCHIVE_ROOT + "/LICENSE",
                    ARCHIVE_ROOT + "/SHA256SUMS",
                }
                if is_windows:
                    with zipfile.ZipFile(archive_path) as archive:
                        self.assertEqual(set(archive.namelist()), expected)
                        def get(member):
                            return archive.read(ARCHIVE_ROOT + "/" + member)
                        self._check_members(get, name, packed_name, dist)
                else:
                    with tarfile.open(archive_path, "r:gz") as archive:
                        self.assertEqual(set(archive.getnames()), expected)
                        def get(member):
                            return archive.extractfile(
                                ARCHIVE_ROOT + "/" + member).read()
                        self._check_members(get, name, packed_name, dist)
                        self.assertEqual(
                            archive.getmember(ARCHIVE_ROOT + "/" + packed_name).mode,
                            0o755)
            # The original seven executables are retained in dist/.
            self.assertEqual(
                {p.name for p in dist.iterdir()},
                set(BINARIES) | {"releases"})

    def _check_members(self, get, source_name, packed_name, dist):
        binary = (dist / source_name).read_bytes()
        self.assertEqual(get(packed_name), binary)
        self.assertEqual(
            get("SHA256SUMS").decode("ascii"),
            hashlib.sha256(binary).hexdigest() + "  " + packed_name + "\n")
        self.assertIn(
            "Project (https://example.org)",
            get("README.txt").decode("utf-8"))
        self.assertEqual(
            get("LICENSE").decode("utf-8"), "GPLv3 test fixture\n")

    def test_extracted_executable_names_have_version_but_no_platform(self):
        version = "v1.0.0-beta.1"
        for label in ARCHIVE_LABELS:
            name = release_executable_name(version, label)
            expected = ("tkx4xtoolkit-v1_0_0-beta_1.exe" if
                        label.startswith("windows-") else
                        "tkx4xtoolkit-v1_0_0-beta_1")
            self.assertEqual(name, expected)
            self.assertNotIn("macos", name)
            self.assertNotIn("arm64", name)
            self.assertNotIn("x86_64", name)

    def test_missing_both_macos_builds_creates_five_archives(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            dist = root / "dist"
            dist.mkdir()
            for name in BINARIES[:5]:
                (dist / name).write_bytes(("fake build: " + name).encode())
            (root / "README.md").write_text(
                "# Toolkit\n\n[Project](https://example.org)\n", encoding="utf-8")
            (root / "LICENSE").write_text("GPLv3 test fixture\n", encoding="utf-8")
            archives = package(dist, "v1.0.0-beta.1", root)
            self.assertEqual(len(archives), 5)
            self.assertEqual(
                [p.name for p in archives],
                ["tkx4xtoolkit-v1.0.0-beta.1-" + label +
                 (".zip" if label.startswith("windows-") else ".tar.gz")
                 for label in ARCHIVE_LABELS[:5]])
            for name, label, archive_path in zip(BINARIES[:5],
                                                  ARCHIVE_LABELS[:5], archives):
                packed_name = release_executable_name("v1.0.0-beta.1", label)
                if label.startswith("windows-"):
                    with zipfile.ZipFile(archive_path) as archive:
                        self._check_members(
                            lambda member: archive.read(ARCHIVE_ROOT + "/" + member),
                            name, packed_name, dist)
                else:
                    with tarfile.open(archive_path, "r:gz") as archive:
                        self._check_members(
                            lambda member: archive.extractfile(
                                ARCHIVE_ROOT + "/" + member).read(),
                            name, packed_name, dist)
            self.assertEqual({p.name for p in dist.iterdir()},
                             set(BINARIES[:5]) | {"releases"})

    def test_single_macos_binary_is_an_incomplete_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            dist = root / "dist"
            dist.mkdir()
            for name in BINARIES[:-1]:
                (dist / name).write_bytes(b"nonempty")
            (root / "README.md").write_text("# Toolkit\n", encoding="utf-8")
            (root / "LICENSE").write_text("License", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Incomplete macOS build"):
                package(dist, "v1.0.0-beta.1", root)
            self.assertFalse((dist / "releases").exists())

    def test_missing_binary_refuses_any_archives(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            dist = root / "dist"
            dist.mkdir()
            for name in BINARIES[1:]:
                (dist / name).write_bytes(b"nonempty")
            (root / "README.md").write_text("# Test\n")
            (root / "LICENSE").write_text("License\n")
            with self.assertRaisesRegex(ValueError, BINARIES[0]):
                package(dist, "v1.0.0-beta.1", root)
            self.assertFalse((dist / "releases").exists())

    @unittest.skipUnless(shutil.which("make"), "GNU Make required")
    def test_dist_skips_unreachable_remote_mac(self):
        result, archives = self._run_synthetic_dist(mac_build_fails=False)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("Remote Mac unavailable", result.stdout)
        self.assertIn("Created 5 of 7", result.stdout)
        self.assertEqual(len(archives), 5)

    @unittest.skipUnless(shutil.which("make"), "GNU Make required")
    def test_dist_fails_for_remote_mac_build_error(self):
        result, archives = self._run_synthetic_dist(mac_build_fails=True)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("Remote Mac unavailable", result.stdout)
        self.assertEqual(archives, [])

    def _run_synthetic_dist(self, mac_build_fails):
        # Use fake compilers and fake SSH to exercise the real Makefile's
        # release control flow, without cross-compilers or network access.
        # Copy the release inputs into a temporary directory.
        # Return the archive filenames before removing the temporary directory.
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            (root / "Makefile").write_bytes((ROOT / "Makefile").read_bytes())
            (root / "tkx4xtoolkit.c").write_text(
                '#define TOOL_VERSION "v1.0.0-beta.1"\n', encoding="utf-8")
            for name in ("firmware_hex.c", "fwtransmute.c", "fwtransmute.h",
                         "fwgrimoire_tk840_9306_8355.h",
                         "tkx4x_plus25_patch.h", "firmware_hex.h"):
                (root / name).write_text("/* test */\n", encoding="utf-8")
            (root / "README.md").write_text("# Test\n", encoding="utf-8")
            (root / "LICENSE").write_text("License\n", encoding="utf-8")
            tool_dir = root / "tools"
            tool_dir.mkdir()
            shutil.copyfile(ROOT / "tools" / "package_release.py",
                            tool_dir / "package_release.py")
            bin_dir = root / "fakebin"
            bin_dir.mkdir()
            fake_cc = bin_dir / "fakecc"
            fake_cc.write_text(
                "#!/bin/sh\n"
                "while [ \"$#\" -gt 0 ]; do\n"
                "  if [ \"$1\" = '-o' ]; then shift; out=\"$1\"; fi\n"
                "  shift\n"
                "done\n"
                "printf 'fake executable\\n' > \"$out\"\n",
                encoding="utf-8")
            fake_cc.chmod(0o755)
            fake_ssh = bin_dir / "ssh"
            if mac_build_fails:
                fake_ssh.write_text(
                    "#!/bin/sh\n"
                    "case \"$*\" in\n"
                    "  *' true') exit 0;;\n"
                    "  *'mkdir -p '*) cat >/dev/null; exit 0;;\n"
                    "  *'make macos-'*) exit 2;;\n"
                    "  *) exit 3;;\n"
                    "esac\n",
                    encoding="utf-8")
            else:
                fake_ssh.write_text("#!/bin/sh\nexit 255\n", encoding="utf-8")
            fake_ssh.chmod(0o755)
            args = ["make", "-j1", "dist", "MAC_USER=test", "MAC_HOST=fake",
                    "CC=" + str(fake_cc), "MUSLCC=" + str(fake_cc),
                    "MUSL32CC=" + str(fake_cc), "WINCC=" + str(fake_cc),
                    "WIN32CC=" + str(fake_cc), "STRIP=true", "WINSTRIP=true",
                    "WIN32STRIP=true"]
            env = os.environ.copy()
            env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
            result = subprocess.run(args, cwd=root, env=env, text=True,
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=25)
            dist = root / "dist"
            archives = sorted(p.name for p in (dist / "releases").iterdir()) if (
                dist / "releases").exists() else []
            return result, archives

    def test_invalid_version(self):
        with self.assertRaises(ValueError):
            package(pathlib.Path("."), "../unsafe", pathlib.Path("."))


if __name__ == "__main__":
    unittest.main()
