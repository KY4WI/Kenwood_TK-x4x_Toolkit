#!/usr/bin/env python3
# Copyright (c) 2026 Dave, KY4WI
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create per-platform release archives from available finished binaries.

Standard library only. Does not compile, strip, or modify the binaries.
"""
import argparse
import hashlib
import io
import re
import tarfile
import textwrap
import zipfile
from pathlib import Path

REPO_URL = "https://github.com/KY4WI/Kenwood_TK-x4x_Toolkit"
ARCHIVE_ROOT = "tkx4xtoolkit"
BINARIES = (
    "tkx4xtoolkit-linux-x86_64",
    "tkx4xtoolkit-linux-x86_64-static",
    "tkx4xtoolkit-linux-x86_32-static",
    "tkx4xtoolkit-windows-x86_64.exe",
    "tkx4xtoolkit-windows-x86_32.exe",
    "tkx4xtoolkit-x86_64.macos",
    "tkx4xtoolkit-arm64.macos",
)
# Archive names identify the platform; executables inside carry only the version.
ARCHIVE_LABELS = (
    "linux-x86_64",
    "linux-x86_64-static",
    "linux-x86_32-static",
    "windows-x86_64",
    "windows-x86_32",
    "macos-x86_64",
    "macos-arm64",
)
REQUIRED_BINARIES = BINARIES[:5]
MACOS_BINARIES = BINARIES[5:]
_IMAGE = re.compile(r'<img\b[^>]*\bsrc="([^"]+)"[^>]*>', re.I)
_LINK = re.compile(r'\[([^\]]+)\]\(([^)]+)\)')
_HEADING = re.compile(r'^(#{1,4})\s+(.+?)\s*$')


def markdown_to_text(markdown):
    """Make an 80-column README.txt; preserve code and unsplittable URLs."""
    lines = []
    paragraph = []
    table = []
    in_code = False
    pending_item = None
    width = 80

    def inline(value):
        def link(match):
            address = match.group(2)
            if not address.startswith(("https://", "http://")):
                address = REPO_URL + "/blob/main/" + address
            return "%s (%s)" % (match.group(1), address)

        return _LINK.sub(link, value).replace("**", "").replace("`", "")

    def wrap(value, first="", continuation=""):
        # Never insert whitespace into a URL or a shell command to hit 80.
        # An indivisible URL can therefore occupy a longer line.
        return textwrap.wrap(
            value, width=width, initial_indent=first,
            subsequent_indent=continuation,
            break_long_words=False, break_on_hyphens=False,
        )

    def flush_item():
        nonlocal pending_item
        if pending_item is not None:
            prefix, parts = pending_item
            lines.extend(wrap(" ".join(parts), first=prefix,
                              continuation=" " * len(prefix)))
            pending_item = None

    def flush_paragraph():
        if paragraph:
            value = " ".join(paragraph)
            # README.md displays the image immediately below; README.txt
            # displays the diagram URL instead, so avoid implying an image.
            value = value.replace(" A wiring diagram is provided below.", "")
            lines.extend(wrap(value))
            paragraph.clear()

    def flush_table():
        if not table:
            return
        headers = table[0]
        def is_separator(row):
            return len(row) == len(headers) and all(
                re.fullmatch(r":?-{3,}:?", v.replace(" ", "")) for v in row
            )
        body = table[2:] if len(table) > 1 and is_separator(table[1]) else table[1:]
        body = [row for row in body if len(row) == len(headers)]
        widths = [max(len(row[i]) for row in [headers] + body)
                  for i in range(len(headers))]
        if sum(widths) + 3 * (len(headers) - 1) <= width:
            def format_row(row):
                return " | ".join(v.ljust(w) for v, w in zip(row, widths)).rstrip()
            lines.append(format_row(headers))
            lines.append("-+-".join("-" * w for w in widths))
            for row in body:
                lines.append(format_row(row))
        else:
            # Wide Markdown tables are more readable as labeled records.
            for index, row in enumerate(body):
                if index:
                    lines.append("")
                for header, value in zip(headers, row):
                    label = header + ": "
                    lines.extend(wrap(value, first="  " + label,
                                      continuation=" " * (2 + len(label))))
        table.clear()

    for line in markdown.splitlines():
        if line.lstrip().startswith("```"):
            flush_item()
            flush_paragraph()
            flush_table()
            in_code = not in_code
            continue
        if in_code:
            lines.append("    " + line if line else "")
            continue

        stripped = line.strip()
        if stripped.startswith("|") and stripped.endswith("|"):
            flush_item()
            flush_paragraph()
            table.append([inline(c.strip()) for c in stripped[1:-1].split("|")])
            continue
        flush_table()

        if not stripped:
            flush_item()
            flush_paragraph()
            if lines and lines[-1]:
                lines.append("")
            continue

        picture = _IMAGE.search(line)
        if picture:
            flush_item()
            flush_paragraph()
            address = picture.group(1)
            if not address.startswith(("https://", "http://")):
                address = REPO_URL + "/blob/main/" + address
            lines.append("Programming cable diagram:")
            lines.extend(wrap(address))
            continue

        heading = _HEADING.match(line)
        if heading:
            flush_item()
            flush_paragraph()
            label = inline(heading.group(2))
            if lines and lines[-1]:
                lines.append("")
            lines.extend(wrap(label))
            lines.append(("=" if len(heading.group(1)) <= 2 else "-") *
                         min(len(label), width))
            lines.append("")
            continue

        item = re.match(r"^(\s*(?:[-*+] |\d+\. ))(.*)$", line)
        if item:
            flush_item()
            flush_paragraph()
            pending_item = (item.group(1), [inline(item.group(2))])
            continue

        # Markdown continuation lines belonging to a numbered/bulleted item
        # must wrap under the item's text, not become an independent paragraph.
        if pending_item is not None:
            prefix, parts = pending_item
            indentation = len(line) - len(line.lstrip())
            if indentation >= len(prefix):
                parts.append(inline(stripped))
                continue
            flush_item()

        quote = re.match(r"^\s*>\s*(.*)$", line)
        if quote:
            flush_paragraph()
            lines.extend(wrap(inline(quote.group(1)),
                              first="> ", continuation="> "))
            continue

        paragraph.append(inline(stripped))

    flush_item()
    flush_paragraph()
    flush_table()
    return "\n".join(lines).rstrip() + "\n"


def _add_bytes(archive, name, data, mode=0o644):
    info = tarfile.TarInfo(ARCHIVE_ROOT + "/" + name)
    info.size = len(data)
    info.mode = mode
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    archive.addfile(info, io.BytesIO(data))


def _zip_bytes(archive, name, data, mode=0o644):
    info = zipfile.ZipInfo(ARCHIVE_ROOT + "/" + name)
    info.date_time = (1980, 1, 1, 0, 0, 0)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    info.external_attr = (0o100000 | mode) << 16
    archive.writestr(info, data, compresslevel=9)


def release_executable_name(version, label):
    """Keep the version, but not the platform, in an extracted executable name.

    Use underscores for version dots so macOS does not mistake the final
    version component for a filename extension.
    """
    name = "tkx4xtoolkit-" + version.replace(".", "_")
    return name + (".exe" if label.startswith("windows-") else "")


def package(dist_dir, version, project_dir):
    """Package five required builds, plus both macOS builds when available."""
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", version):
        raise ValueError("Invalid release version: %r" % version)

    def present(name):
        file = dist_dir / name
        return file.is_file() and file.stat().st_size > 0

    missing = [name for name in REQUIRED_BINARIES if not present(name)]
    if missing:
        raise ValueError("Missing or empty release binaries: " + ", ".join(missing))

    mac_present = [name for name in MACOS_BINARIES if present(name)]
    if len(mac_present) == 1:
        raise ValueError("Incomplete macOS build; both architectures are required: " +
                         ", ".join(name for name in MACOS_BINARIES if not present(name)))

    readme = markdown_to_text(
        (project_dir / "README.md").read_text(encoding="utf-8")).encode("utf-8")
    license_data = (project_dir / "LICENSE").read_bytes()
    release_dir = dist_dir / "releases"
    release_dir.mkdir(exist_ok=True)
    outputs = []
    try:
        for binary_name, label in zip(BINARIES, ARCHIVE_LABELS):
            if binary_name in MACOS_BINARIES and not mac_present:
                continue
            binary = (dist_dir / binary_name).read_bytes()
            packed_name = release_executable_name(version, label)
            checksum = (hashlib.sha256(binary).hexdigest() +
                        "  " + packed_name + "\n").encode("ascii")
            members = ((packed_name, binary, 0o755),
                       ("README.txt", readme, 0o644),
                       ("LICENSE", license_data, 0o644),
                       ("SHA256SUMS", checksum, 0o644))
            is_windows = label.startswith("windows-")
            suffix = ".zip" if is_windows else ".tar.gz"
            output = release_dir / ("tkx4xtoolkit-" + version + "-" +
                                    label + suffix)
            temporary = release_dir / (output.name + ".tmp")
            try:
                if is_windows:
                    with zipfile.ZipFile(temporary, "w") as archive:
                        for name, data, mode in members:
                            _zip_bytes(archive, name, data, mode)
                else:
                    with tarfile.open(temporary, "w:gz", compresslevel=9) as archive:
                        for name, data, mode in members:
                            _add_bytes(archive, name, data, mode)
                temporary.replace(output)
                outputs.append(output)
            finally:
                if temporary.exists():
                    temporary.unlink()
    except Exception:
        for output in outputs:
            output.unlink(missing_ok=True)
        raise
    return outputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist-dir", required=True, type=Path)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    try:
        outputs = package(args.dist_dir, args.version, root)
    except (ValueError, OSError, tarfile.TarError, zipfile.BadZipFile) as exc:
        parser.exit(1, "Release packaging failed: %s\n" % exc)
    print("Created %d of %d platform-specific release archives:" %
          (len(outputs), len(BINARIES)))
    for output in outputs:
        print("  %s" % output)
    if len(outputs) < len(BINARIES):
        print("WARNING: macOS builds unavailable; both macOS archives omitted.")


if __name__ == "__main__":
    main()
