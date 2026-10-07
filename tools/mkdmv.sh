#!/bin/sh
#
# tools/mkdmv.sh - build the DMV-native (no-DOS) GentleOS/16 disk image.
#
# Run this AFTER the normal build has produced GT16.COM and GT16.DAT (e.g. via
# the DOSBox/wmake build, or any toolchain that writes those two files to the
# source root). It assembles the DMV boot sector and lays out the native image:
#
#   sector 0        bootdmv  (16BIT signature, loads kernel+initrd, hands off)
#   sectors 1..     GT16.COM (kernel)   -> 0x1000:0x0100
#   sectors ..end   GT16.DAT (initrd)   -> 0x3000:0x0000
#
# Output: GT16DMV.IMG (360 KB, the real DMV floppy geometry: 40 tracks x
# 2 heads x 9 sectors x 512 B).
#
# The DOS .com build and its PC-style images are produced by the normal
# tools/mkdisks.pl; this script only adds the native image.

set -e

cd "$(dirname "$0")/.."

if [ ! -f GT16.COM ] || [ ! -f GT16.DAT ]; then
    echo "error: build GT16.COM and GT16.DAT first (run the normal build)." >&2
    exit 1
fi

mkdir -p build/boot_dmv
echo "Assembling DMV boot sector..."
nasm -f bin -o build/boot_dmv/bootdmv.bin boot_dmv/bootdmv.s

echo "Building native disk image(s)..."
perl tools/mkdisks_dmv.pl

echo "Done. Native image: GT16DMV.IMG (360 KB)"
