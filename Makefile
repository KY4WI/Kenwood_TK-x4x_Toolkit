# Copyright (c) 2026 Dave, KY4WI
# SPDX-License-Identifier: GPL-3.0-or-later

# Use up to twelve parallel Make jobs by default. An explicit -j/--jobs
# option on the command line (including -j1) takes precedence.
# This affects independent Make targets, not GCC's own compilation threads.
ifeq ($(filter -j% --jobs%,$(MAKEFLAGS)),)
MAKEFLAGS += -j12
endif

CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=
PYTHON ?= python3

WINCC ?= x86_64-w64-mingw32-gcc
WINCFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
WINLDFLAGS ?=

WIN32CC ?= i686-w64-mingw32-gcc
WIN32CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
WIN32LDFLAGS ?= -static

MUSLCC ?= musl-gcc
MUSLCFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
MUSLLDFLAGS ?= -static

MUSL32CC ?= i386-linux-musl-gcc
MUSL32CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
MUSL32LDFLAGS ?= -static

STRIP ?= strip
WINSTRIP ?= x86_64-w64-mingw32-strip
WIN32STRIP ?= i686-w64-mingw32-strip

# Firmware validation, normalization and conversion are built into the Toolkit.
SRC = tkx4xtoolkit.c firmware_hex.c fwtransmute.c
# Development builds show the exact Git revision and mark uncommitted edits.
# GitHub release binaries omit the identifier with RELEASE_BUILD=1.
RELEASE_BUILD ?= 0
BUILD_GIT_VERSION ?= $(shell git rev-parse --short=8 HEAD 2>/dev/null)
BUILD_DIRTY ?= $(shell git status --porcelain 2>/dev/null | head -n 1)
ifeq ($(RELEASE_BUILD),1)
BUILD_VERSION_FLAGS = -DTOOL_RELEASE_BUILD
else ifneq ($(strip $(BUILD_GIT_VERSION)),)
BUILD_VERSION_FLAGS = -DTOOL_GIT_VERSION=\"$(BUILD_GIT_VERSION)$(if $(strip $(BUILD_DIRTY)),-dirty)\"
else
BUILD_VERSION_FLAGS =
endif
CFLAGS += $(BUILD_VERSION_FLAGS)
WINCFLAGS += $(BUILD_VERSION_FLAGS)
WIN32CFLAGS += $(BUILD_VERSION_FLAGS)
MUSLCFLAGS += $(BUILD_VERSION_FLAGS)
MUSL32CFLAGS += $(BUILD_VERSION_FLAGS)

# Local Mac build-machine settings. This file is intentionally not tracked.
-include mac-credentials.mk

HOST_OS := $(shell uname -s 2>/dev/null)
HOST_ARCH := $(shell uname -m 2>/dev/null)

MAC_USER ?=
MAC_HOST ?=
MAC_DIR  ?= ~/tkx4xtoolkit-build
MAC_REMOTE = $(MAC_USER)@$(MAC_HOST)
# Noninteractive SSH with a bounded connection attempt for remote build jobs.
MAC_SSH_OPTS ?= -o BatchMode=yes -o ConnectTimeout=5 -o ConnectionAttempts=1
MACOS_X86_64_MIN ?= 10.13
MACOS_ARM64_MIN ?= 11.0

# Pull the release version directly from the C source so there is only one
# version string to maintain.
VERSION := $(shell awk '$$2 == "TOOL_VERSION" { gsub(/"/, "", $$3); print $$3; exit }' $(SRC))

ifeq ($(strip $(VERSION)),)
$(error Could not extract TOOL_VERSION from $(SRC))
endif

# Built executable names stay stable across Beta/RC/stable releases.
# Git tags and release archives carry the release version instead.
TARGET = tkx4xtoolkit
STATIC_TARGET = tkx4xtoolkit-static
STATIC32_TARGET = tkx4xtoolkit-x86_32-static
WINTARGET = tkx4xtoolkit.exe
WIN32TARGET = tkx4xtoolkit-x86_32.exe
MACTARGET_X86_64 = tkx4xtoolkit-x86_64.macos
MACTARGET_ARM64 = tkx4xtoolkit-arm64.macos

DIST_DIR = dist
DIST_LINUX_X86_64 = $(DIST_DIR)/tkx4xtoolkit-linux-x86_64
DIST_LINUX_X86_64_STATIC = $(DIST_DIR)/tkx4xtoolkit-linux-x86_64-static
DIST_LINUX_X86_32_STATIC = $(DIST_DIR)/tkx4xtoolkit-linux-x86_32-static
DIST_WINDOWS_X86_64 = $(DIST_DIR)/tkx4xtoolkit-windows-x86_64.exe
DIST_WINDOWS_X86_32 = $(DIST_DIR)/tkx4xtoolkit-windows-x86_32.exe
DIST_MACOS_X86_64 = $(DIST_DIR)/tkx4xtoolkit-x86_64.macos
DIST_MACOS_ARM64 = $(DIST_DIR)/tkx4xtoolkit-arm64.macos

# Native defaults. A plain "make" builds only for the host OS and host CPU.
ifeq ($(HOST_OS),Darwin)
NATIVE_RELEASE = macos
NATIVE_PLAIN = macos-plain
ifeq ($(HOST_ARCH),arm64)
MAC_NATIVE_ARCH = arm64
else ifeq ($(HOST_ARCH),x86_64)
MAC_NATIVE_ARCH = x86_64
else
$(error Unsupported macOS architecture: $(HOST_ARCH))
endif
else ifeq ($(HOST_OS),Linux)
NATIVE_RELEASE = linux
NATIVE_PLAIN = linux-plain
else
$(error Unsupported build host: $(HOST_OS))
endif

all: $(NATIVE_RELEASE)

# Linux release build: fresh build and strip.
linux: FORCE
	@if [ "$(HOST_OS)" != "Linux" ]; then echo "linux target requires a Linux host" >&2; exit 2; fi
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)
	$(STRIP) $(TARGET)

# Fully static Linux build using musl. This is intentionally an explicit target;
# normal "make" continues to build only the normal native executable.
linux-static: FORCE
	@if [ "$(HOST_OS)" != "Linux" ]; then echo "linux-static target requires a Linux host" >&2; exit 2; fi
	$(MUSLCC) $(MUSLCFLAGS) -o $(STATIC_TARGET) $(SRC) $(MUSLLDFLAGS)
	$(STRIP) $(STATIC_TARGET)

# Fully static 32-bit x86 Linux build using Debian's i386 musl cross wrapper.
# "linux32" is the convenient alias because 32-bit Linux is distributed static-only.
linux32: linux32-static

linux32-static: FORCE
	@if [ "$(HOST_OS)" != "Linux" ]; then echo "linux32-static target requires a Linux host" >&2; exit 2; fi
	$(MUSL32CC) $(MUSL32CFLAGS) -o $(STATIC32_TARGET) $(SRC) $(MUSL32LDFLAGS)
	$(STRIP) $(STATIC32_TARGET)

# macOS release builds are separate per CPU architecture.
macos: macos-$(MAC_NATIVE_ARCH)

macos-x86_64: FORCE
	@if [ "$(HOST_OS)" != "Darwin" ]; then echo "macos-x86_64 target requires a macOS host" >&2; exit 2; fi
	$(CC) $(CFLAGS) -arch x86_64 -mmacosx-version-min=$(MACOS_X86_64_MIN) -o $(MACTARGET_X86_64) $(SRC) $(LDFLAGS)
	$(STRIP) $(MACTARGET_X86_64)

macos-arm64: FORCE
	@if [ "$(HOST_OS)" != "Darwin" ]; then echo "macos-arm64 target requires a macOS host" >&2; exit 2; fi
	$(CC) $(CFLAGS) -arch arm64 -mmacosx-version-min=$(MACOS_ARM64_MIN) -o $(MACTARGET_ARM64) $(SRC) $(LDFLAGS)
	$(STRIP) $(MACTARGET_ARM64)

# Windows remains an explicit cross-build target; it is never part of "make".
# Release binaries are stripped.
windows: FORCE
	$(WINCC) $(WINCFLAGS) -o $(WINTARGET) $(SRC) $(WINLDFLAGS)
	$(WINSTRIP) $(WINTARGET)

# 32-bit x86 Windows cross-build. -static prevents dependencies on MinGW
# runtime DLLs; normal Windows system DLL imports remain, as expected.
windows32: FORCE
	$(WIN32CC) $(WIN32CFLAGS) -o $(WIN32TARGET) $(SRC) $(WIN32LDFLAGS)
	$(WIN32STRIP) $(WIN32TARGET)

# Plain builds are unstripped.
plain: $(NATIVE_PLAIN)

linux-plain: FORCE
	@if [ "$(HOST_OS)" != "Linux" ]; then echo "linux-plain target requires a Linux host" >&2; exit 2; fi
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

linux-static-plain: FORCE
	@if [ "$(HOST_OS)" != "Linux" ]; then echo "linux-static-plain target requires a Linux host" >&2; exit 2; fi
	$(MUSLCC) $(MUSLCFLAGS) -o $(STATIC_TARGET) $(SRC) $(MUSLLDFLAGS)

linux32-static-plain: FORCE
	@if [ "$(HOST_OS)" != "Linux" ]; then echo "linux32-static-plain target requires a Linux host" >&2; exit 2; fi
	$(MUSL32CC) $(MUSL32CFLAGS) -o $(STATIC32_TARGET) $(SRC) $(MUSL32LDFLAGS)

macos-plain: macos-$(MAC_NATIVE_ARCH)-plain

macos-x86_64-plain: FORCE
	@if [ "$(HOST_OS)" != "Darwin" ]; then echo "macos-x86_64-plain target requires a macOS host" >&2; exit 2; fi
	$(CC) $(CFLAGS) -arch x86_64 -mmacosx-version-min=$(MACOS_X86_64_MIN) -o $(MACTARGET_X86_64) $(SRC) $(LDFLAGS)

macos-arm64-plain: FORCE
	@if [ "$(HOST_OS)" != "Darwin" ]; then echo "macos-arm64-plain target requires a macOS host" >&2; exit 2; fi
	$(CC) $(CFLAGS) -arch arm64 -mmacosx-version-min=$(MACOS_ARM64_MIN) -o $(MACTARGET_ARM64) $(SRC) $(LDFLAGS)

windows-plain: FORCE
	$(WINCC) $(WINCFLAGS) -o $(WINTARGET) $(SRC) $(WINLDFLAGS)

windows32-plain: FORCE
	$(WIN32CC) $(WIN32CFLAGS) -o $(WIN32TARGET) $(SRC) $(WIN32LDFLAGS)

# Remote macOS builder. "make mac-remote" builds both release architectures.
# The ARM64 binary can be cross-compiled on an Intel Mac with a suitable SDK,
# but it must be runtime-tested on actual Apple Silicon hardware.
mac-upload: FORCE
	@if [ -z "$(MAC_USER)" ] || [ -z "$(MAC_HOST)" ]; then \
		echo "Set MAC_USER and MAC_HOST in mac-credentials.mk (see mac-credentials.mk.example)." >&2; \
		exit 2; \
	fi
	tar cf - $(SRC) fwtransmute.h fwgrimoire_tk840_9306_8355.h \
	  tkx4x_plus25_patch.h Makefile firmware_hex.h | ssh -x $(MAC_SSH_OPTS) $(MAC_REMOTE) 'mkdir -p $(MAC_DIR) && tar xf - -C $(MAC_DIR)'

mac-remote-x86_64: mac-upload
	ssh -x $(MAC_SSH_OPTS) $(MAC_REMOTE) 'cd $(MAC_DIR) && make macos-x86_64 BUILD_GIT_VERSION="$(BUILD_GIT_VERSION)" BUILD_DIRTY="$(if $(strip $(BUILD_DIRTY)),1,)" RELEASE_BUILD="$(RELEASE_BUILD)"'
	scp $(MAC_SSH_OPTS) $(MAC_REMOTE):$(MAC_DIR)/$(MACTARGET_X86_64) .

mac-remote-arm64: mac-upload
	ssh -x $(MAC_SSH_OPTS) $(MAC_REMOTE) 'cd $(MAC_DIR) && make macos-arm64 BUILD_GIT_VERSION="$(BUILD_GIT_VERSION)" BUILD_DIRTY="$(if $(strip $(BUILD_DIRTY)),1,)" RELEASE_BUILD="$(RELEASE_BUILD)"'
	scp $(MAC_SSH_OPTS) $(MAC_REMOTE):$(MAC_DIR)/$(MACTARGET_ARM64) .

mac-remote: mac-remote-x86_64 mac-remote-arm64

# Build Linux/Windows release binaries from the Linux host. If the remote Mac
# is reachable, also build both macOS architectures. A missing Mac results in
# five archives with a warning; an actual Mac build failure stops the release.
# Individually named build outputs stay in dist/; archives go to dist/releases/.
dist: FORCE
	@if [ "$(HOST_OS)" != "Linux" ]; then echo "dist target requires a Linux host" >&2; exit 2; fi
	rm -rf $(DIST_DIR)
	mkdir -p $(DIST_DIR)
	$(MAKE) linux RELEASE_BUILD=1
	cp $(TARGET) $(DIST_LINUX_X86_64)
	$(MAKE) linux-static RELEASE_BUILD=1
	cp $(STATIC_TARGET) $(DIST_LINUX_X86_64_STATIC)
	$(MAKE) linux32-static RELEASE_BUILD=1
	cp $(STATIC32_TARGET) $(DIST_LINUX_X86_32_STATIC)
	$(MAKE) windows RELEASE_BUILD=1
	cp $(WINTARGET) $(DIST_WINDOWS_X86_64)
	$(MAKE) windows32 RELEASE_BUILD=1
	cp $(WIN32TARGET) $(DIST_WINDOWS_X86_32)
	@if [ -z "$(MAC_USER)" ] || [ -z "$(MAC_HOST)" ]; then \
		echo "WARNING: Remote Mac not configured; omitting both macOS builds."; \
	elif ssh -x $(MAC_SSH_OPTS) $(MAC_REMOTE) true >/dev/null 2>&1; then \
		$(MAKE) mac-remote RELEASE_BUILD=1 || exit $$?; \
		cp $(MACTARGET_X86_64) $(DIST_MACOS_X86_64) || exit $$?; \
		cp $(MACTARGET_ARM64) $(DIST_MACOS_ARM64) || exit $$?; \
	else \
		echo "WARNING: Remote Mac unavailable (or SSH authentication failed); omitting both macOS builds."; \
	fi
	$(PYTHON) tools/package_release.py --dist-dir "$(DIST_DIR)" --version "$(VERSION)"
	@printf '\nIndividual binaries in %s/; upload-ready archives in %s/releases/:\n' "$(DIST_DIR)" "$(DIST_DIR)"
	@ls -1 $(DIST_DIR)/releases

dist-clean:
	rm -rf $(DIST_DIR)

clean:
	rm -f $(TARGET) $(STATIC_TARGET) $(STATIC32_TARGET) $(WINTARGET) $(WIN32TARGET) $(MACTARGET_X86_64) $(MACTARGET_ARM64) \
		tkx4xtoolkit tkx4xtoolkit-static tkx4xtoolkit.exe \
		tkx4xtoolkit-x86-static tkx4xtoolkit-x86.exe \
		tkx4xtoolkit-x86_64.macos tkx4xtoolkit-arm64.macos \
		tkx4xtoolkit-Beta_* tkx4xtoolkit-Beta_*.exe tkx4xtoolkit-Beta_*.macos \
		tkx4xtoolkit-RC* tkx4xtoolkit-RC*.exe tkx4xtoolkit-RC*.macos \
		$(DIST_LINUX_X86_64) $(DIST_LINUX_X86_64_STATIC) $(DIST_LINUX_X86_32_STATIC) \
		$(DIST_WINDOWS_X86_64) $(DIST_WINDOWS_X86_32) $(DIST_MACOS_X86_64) $(DIST_MACOS_ARM64)
	@rmdir $(DIST_DIR) 2>/dev/null || true

FORCE:

.PHONY: all linux linux-static linux32 linux32-static macos macos-x86_64 macos-arm64 windows windows32 \
	plain linux-plain linux-static-plain linux32-static-plain macos-plain macos-x86_64-plain \
	macos-arm64-plain windows-plain windows32-plain mac-upload mac-remote mac-remote-x86_64 \
	mac-remote-arm64 dist dist-clean clean FORCE