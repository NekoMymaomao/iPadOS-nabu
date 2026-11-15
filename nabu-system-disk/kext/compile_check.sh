#!/bin/sh
# Cross-compile the driver for the 8796 kernel ABI (arm64, iOS 16.5).
#
# This is the same target/flags as build_nabu_system_disk.py; it exists so the
# sources can be compile-checked on a Linux host (or CI) without xcrun.  It does
# not replace the macOS build: the prelink step still needs a macOS toolchain.
#
# Required:
#   EXPORT_HDRS          an exported kernel header set (osfmk/bsd/libkern/...)
#   IOKIT_STORAGE_INC    directory holding IOKit/storage/*.h and TargetConditionals.h
#   NABU_STORAGE_INC     directory holding NabuStorageApi.h
# Optional:
#   XNU_EXTERNAL_HEADERS the xnu tree's EXTERNAL_HEADERS directory
#   OUT                  output directory (default: build/kext)
set -eu

: "${EXPORT_HDRS:?set EXPORT_HDRS to an EXPORT_HDRS tree}"
: "${IOKIT_STORAGE_INC:?set IOKIT_STORAGE_INC to IOKit/storage headers}"
: "${NABU_STORAGE_INC:?set NABU_STORAGE_INC to the nabu-storage headers}"

here=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-$here/build/kext}
mkdir -p "$out"

inc=""
for d in osfmk bsd libkern pexpert iokit security san; do
    [ -d "$EXPORT_HDRS/$d" ] && inc="$inc -I$EXPORT_HDRS/$d"
done
[ -n "${XNU_EXTERNAL_HEADERS:-}" ] && inc="$inc -I$XNU_EXTERNAL_HEADERS"

compile() {
    # shellcheck disable=SC2086
    clang++ -target arm64-apple-ios16.5 -std=gnu++17 -Os -mkernel -fapple-kext \
        -fno-exceptions -fno-rtti -fno-builtin -fno-stack-protector \
        -fno-unwind-tables -fno-asynchronous-unwind-tables \
        -nostdinc -nostdinc++ -DKERNEL=1 -D__DARWIN_64_BIT_INO_T=1 \
        -DARM64_BOARD_CONFIG_BCM2837 -DXNU_TARGET_OS_IOS=1 \
        -Wno-deprecated-declarations -Wno-inconsistent-missing-override \
        -I"$IOKIT_STORAGE_INC" -I"$NABU_STORAGE_INC" -I"$here/src" $inc \
        -c "$1" -o "$2"
}

compile "$here/src/NabuDiskCore.cpp"       "$out/NabuDiskCore.o"
compile "$here/src/NabuSystemDisk.cpp"     "$out/NabuSystemDisk.o"
compile "$here/kext/NabuSystemDiskKmod.cpp" "$out/NabuSystemDiskKmod.o"

for f in "$out"/*.o; do
    file "$f"
done
printf '%s\n' "kmod symbols:"
if command -v llvm-nm >/dev/null 2>&1; then nm_tool=llvm-nm
elif command -v llvm-nm-18 >/dev/null 2>&1; then nm_tool=llvm-nm-18
else nm_tool=nm; fi
"$nm_tool" "$out/NabuSystemDiskKmod.o" | grep -E '_kmod_info|_nabu_system_disk_start' || true
