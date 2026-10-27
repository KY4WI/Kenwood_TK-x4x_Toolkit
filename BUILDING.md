# Building TKx4xToolkit from source

These instructions are for developers using the source repository, **not**
for the prebuilt release archives. The release archives contain executables
and documentation but no compiler or build files.

## Building

Development builds show `v1.0.0-beta.1 (git abcdef12)`, with `-dirty` when
the source tree contains uncommitted changes. Official binary release
builds use `RELEASE_BUILD=1` to omit the Git identifier; `make dist`
automatically uses that setting.


The version is read directly from `TOOL_VERSION` in `tkx4xtoolkit.c`. Local build executable filenames remain stable across Beta, RC, and stable releases. Examples are `tkx4xtoolkit`, `tkx4xtoolkit-static`, `tkx4xtoolkit-x86_32-static`, `tkx4xtoolkit.exe`, `tkx4xtoolkit-x86_32.exe`, `tkx4xtoolkit-x86_64.macos`, and `tkx4xtoolkit-arm64.macos`. Git tags and archive filenames also carry the release version. Within each archive, the executable is renamed to `tkx4xtoolkit-v1_0_0-beta_1` (or `tkx4xtoolkit-v1_0_0-beta_1.exe` on Windows) so that copies outside their archives remain identifiable. Archive members omit platform and CPU identifiers; macOS executables have no `.macos` extension.

### Native release build

```sh
make
```

or:

```sh
make all
```

`make` builds **only for the current host OS and CPU**. On Linux it builds and strips the normal dynamically linked binary. On macOS it builds and strips the native architecture `.macos` Mach-O binary.

Explicit native targets are also available:

```sh
make linux
make macos
```

### Static Linux build

A fully static Linux executable can be built with musl:

```sh
make linux-static
```

This uses `musl-gcc`, links with `-static`, and strips the result. The unstripped form is:

```sh
make linux-static-plain
```

On Debian/Ubuntu, `musl-gcc` is provided by the `musl-tools` package. `MUSLCC`, `MUSLCFLAGS`, and `MUSLLDFLAGS` can be overridden on the `make` command line.

A fully static 32-bit x86 Linux build is also available:

```sh
make linux32
```

`linux32` is an alias for `linux32-static`. The output filename uses `x86_32` to make its architecture explicit. The plain, unstripped form is `make linux32-static-plain`. The default compiler wrapper is `i386-linux-musl-gcc`; on Debian this is provided by the i386 musl development package.

### Windows cross-build

Windows is always an explicit target and is never built by plain `make`:

```sh
make windows
```

It uses `x86_64-w64-mingw32-gcc` and `x86_64-w64-mingw32-strip` by default. The release executable is stripped.

A static 32-bit Windows build is available with:

```sh
make windows32
```

It uses `i686-w64-mingw32-gcc` with `-static` and strips the result. The output filename uses `x86_32`; `make windows32-plain` builds the unstripped form.

### macOS architecture builds

Intel and Apple-silicon binaries are kept separate:

```sh
make macos-x86_64
make macos-arm64
```

Intel builds default to a macOS 10.13 deployment target. ARM64 builds default to macOS 11.0. Override these with `MACOS_X86_64_MIN=...` or `MACOS_ARM64_MIN=...` if needed.

### Plain builds

```sh
make plain
```

This builds only for the current host OS and CPU **without stripping**. Explicit plain targets are:

```sh
make linux-plain
make linux-static-plain
make linux32-static-plain
make macos-x86_64-plain
make macos-arm64-plain
make windows-plain
make windows32-plain
```

Compilers and tools can be overridden with Make variables such as `CC=...`, `WINCC=...`, `MUSLCC=...`, `STRIP=...`, and `WINSTRIP=...`.

### Remote macOS build from Linux

From the Linux development machine:

```sh
make mac-remote
```

This uploads the source and Makefile to the configured Mac, builds and strips **both** the Intel and ARM64 macOS executables, and downloads both `.macos` files. The ARM64 binary can be cross-compiled by a suitable Intel Mac toolchain, but it still needs runtime testing on actual Apple-silicon hardware.

Individual remote targets are also available:

```sh
make mac-remote-x86_64
make mac-remote-arm64
```

Remote Mac settings are normally kept in the untracked `mac-credentials.mk` file. See `mac-credentials.mk.example` for `MAC_USER`, `MAC_HOST`, and `MAC_DIR`. They may also be overridden on the `make` command line. SSH uses noninteractive authentication and a five-second connection timeout by default (overridable with `MAC_SSH_OPTS`). Direct `make mac-remote` remains an error when the remote build fails.


### Build all release binaries

From the Linux release machine:

```sh
make dist
```

This freshly builds and strips the normal Linux x86_64 binary, static Linux x86_64 and x86_32 binaries, and Windows x86_64 and x86_32 binaries. When the remote Mac is reachable, it also builds both macOS architectures.

**If the remote Mac is offline, not configured, or SSH authentication fails**, `make dist` prints a warning and still successfully creates the five Linux/Windows archives. When the remote Mac is available, it creates all seven archives. A remote Mac build, upload, or download failure **after a successful connection** remains a build error and does not masquerade as an offline machine. Re-run `make dist` once the Mac is available to rebuild all seven.

The platform-specific local build outputs remain in `dist/`, and the separate, versioned archives are created under `dist/releases/` for uploading individually as GitHub Release assets. Each archive name includes the version, OS, and CPU architecture. Inside each archive, the executable contains the version but not the OS or CPU, and it has no `.macos` extension: `tkx4xtoolkit-v1_0_0-beta_1` on Linux/macOS or `tkx4xtoolkit-v1_0_0-beta_1.exe` on Windows. `SHA256SUMS` references the executable's extracted name.

Use `make dist-clean` to remove the generated `dist/` directory.

## Publishing GitHub Releases

After `make dist`, upload the versioned archives from `dist/releases/` as individual GitHub Release assets. If the Mac was unavailable, only the first five archives below will exist; do not describe that partial set as a complete seven-platform release. For v1.0.0-beta.1, the expected archive names are:

- `tkx4xtoolkit-v1.0.0-beta.1-linux-x86_64.tar.gz`
- `tkx4xtoolkit-v1.0.0-beta.1-linux-x86_64-static.tar.gz`
- `tkx4xtoolkit-v1.0.0-beta.1-linux-x86_32-static.tar.gz`
- `tkx4xtoolkit-v1.0.0-beta.1-windows-x86_64.zip`
- `tkx4xtoolkit-v1.0.0-beta.1-windows-x86_32.zip`
- `tkx4xtoolkit-v1.0.0-beta.1-macos-x86_64.tar.gz`
- `tkx4xtoolkit-v1.0.0-beta.1-macos-arm64.tar.gz`

Each archive contains one `tkx4xtoolkit/` directory with **only the matching executable**, plus `README.txt` (plain-text conversion of this README), `LICENSE` (GPLv3), and `SHA256SUMS` (checksum for that executable; check from inside the extracted directory with `sha256sum -c SHA256SUMS` where available).

The programming cable image is not included; `README.txt` links directly to the diagram in this repository. Build tools, firmware files, local build credentials, and other development files are excluded. Linux and macOS use `.tar.gz`; Windows uses `.zip`.

GitHub's release source archives are separate from these binary packages. Publish the matching source alongside the binaries so recipients can obtain the corresponding GPL-covered source code.

Copyright (c) 2026 Dave, KY4WI. Licensed under GPL-3.0-or-later.
