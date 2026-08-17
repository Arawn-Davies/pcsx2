#!/bin/sh
# Build kernelreloaded's kloader.elf and drop it where whiterhino/'s CMake
# build already looks for bundled resources.
#
# whiterhino/pcsx2/CMakeLists.txt does
#     file(GLOB_RECURSE RESOURCE_FILES ${CMAKE_SOURCE_DIR}/bin/resources/*)
# at configure time and copies whatever it finds there into the built
# pcsx2-qt's own resources/ directory -- no CMakeLists.txt change needed on
# either platform, as long as the file exists *before* cmake configures. That
# is the only thing this script is responsible for.
#
# kload needs kloader.elf specifically, not loader.elf or kernel.elf: it is
# the only one of the three that boots (see kernelreloaded's own CLAUDE.md,
# "kloader.elf and loader.elf are the same program"). It is never committed
# -- kernelreloaded's own rule against committing build artifacts applies
# transitively to whiterhino/ bundling one -- so this always builds fresh.
#
# Run from anywhere; it locates the kernelreloaded root relative to itself.
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
KRELOADED_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
WHITERHINO_ROOT="$KRELOADED_ROOT/whiterhino"
DEST_DIR="$WHITERHINO_ROOT/bin/resources/kernelreloaded"

if [ ! -d "$WHITERHINO_ROOT/pcsx2" ]; then
	echo "error: $WHITERHINO_ROOT does not look like the whiterhino/ submodule (no pcsx2/)." >&2
	echo "       git submodule update --init whiterhino" >&2
	exit 1
fi

cd "$KRELOADED_ROOT"

if ! docker image inspect kernelreloaded:local >/dev/null 2>&1; then
	echo "=== building kernelreloaded:local toolchain image (first run only)"
	docker build -t kernelreloaded:local .
fi

# Default FAKE_EXTRA_RAM (64), deliberately not blanked in both builds below:
# this kloader.elf only ever runs under WhiteRhino, never on real hardware, so
# it should claim the memory PCSX2's ExtraMemory=true actually backs -- see
# CLAUDE.md's "Pretending to have more RAM". Blanking it is for the
# real-hardware build.
mkdir -p "$DEST_DIR"

echo "=== building kloader.elf"
./build.sh
cp bin/kloader.elf "$DEST_DIR/kloader.elf"
echo "=== installed: $DEST_DIR/kloader.elf"

# INSTANT_BOOT_DEFAULT: bootlogBegin() at the first line of main(), before
# config.txt is even readable -- see config.mk's own comment. -kload-instant
# uses this instead of the normal kloader.elf's runtime AutoBootTime=-1 path,
# which cannot start the boot log any earlier than config.txt becomes
# readable.
echo "=== building kloader-instant.elf"
./build.sh INSTANT_BOOT_DEFAULT=1
cp bin/kloader.elf "$DEST_DIR/kloader-instant.elf"
echo "=== installed: $DEST_DIR/kloader-instant.elf"
