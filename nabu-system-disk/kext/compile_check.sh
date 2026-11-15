#!/bin/sh
# Cross-compile the drop-in storage driver for the 8796 kernel ABI.
#
# Produces ONE object, src/NabuSystemDisk.cpp (the routing engine in
# NabuDiskCore.cpp is compiled into the same translation unit), which replaces
# NabuDataBlock.o in the existing prelink object list.  The kmod object is
# unchanged, so the bundle id stays org.nabu.driver.DataBlock.
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

# clang's own <stdarg.h>, <stdint.h>, <stddef.h> live in its resource directory,
# which -nostdinc removes along with the system paths.  Put it back explicitly,
# otherwise a hosted toolchain build dies on 'stdarg.h' file not found.
builtin_inc=$("${CXX:-clang++}" -print-resource-dir)/include
[ -d "$builtin_inc" ] && inc="$inc -isystem $builtin_inc"

# Prefer the iOS SDK as sysroot; falling back to the host SDK only produces a
# warning when the target is arm64-apple-ios.
sdk="${IPHONEOS_SDK:-}"
if [ -z "$sdk" ] && command -v xcrun >/dev/null 2>&1; then
    sdk=$(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null || true)
fi
sysroot=""
[ -n "$sdk" ] && [ -d "$sdk" ] && sysroot="-isysroot $sdk"
# -nostdinc also drops the sysroot's own usr/include, so name it explicitly.
# The EXPORT_HDRS trees are -I and therefore still win the lookup.
if [ -n "$sysroot" ] && [ -d "$sdk/usr/include" ]; then
    inc="$inc -isystem $sdk/usr/include"
fi

# The storage headers are kept flat so they can be diffed; the driver includes
# them as <IOKit/storage/...>, so mirror them into that shape.
if [ -d "$IOKIT_STORAGE_INC" ] && [ ! -d "$IOKIT_STORAGE_INC/IOKit/storage" ]; then
    shim="$out/shim"
    mkdir -p "$shim/IOKit/storage"
    for h in "$IOKIT_STORAGE_INC"/*.h; do
        [ -e "$h" ] && cp -f "$h" "$shim/IOKit/storage/"
    done
    inc="$inc -I$shim"
fi

# shellcheck disable=SC2086
clang++ -target arm64-apple-ios16.5 -std=gnu++17 -Os -g -mkernel -fapple-kext \
    $sysroot \
    -fno-exceptions -fno-rtti -fno-builtin -fno-stack-protector \
    -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -nostdinc -nostdinc++ -DKERNEL=1 -D__DARWIN_64_BIT_INO_T=1 \
    -DARM64_BOARD_CONFIG_BCM2837 -DXNU_TARGET_OS_IOS=1 \
    -Wno-deprecated-declarations -Wno-inconsistent-missing-override \
    -I"$IOKIT_STORAGE_INC" -I"$NABU_STORAGE_INC" -I"$here/src" $inc \
    -c "$here/src/NabuSystemDisk.cpp" -o "$out/NabuSystemDisk.o"

file "$out/NabuSystemDisk.o"
if command -v llvm-nm >/dev/null 2>&1; then nm_tool=llvm-nm
elif command -v llvm-nm-18 >/dev/null 2>&1; then nm_tool=llvm-nm-18
else nm_tool=nm; fi
printf '%s\n' "defined:"
"$nm_tool" --defined-only "$out/NabuSystemDisk.o" | grep -E '_nabu_data_block_start|NabuSystemDisk' | head -8 || true
printf '%s\n' "undefined (resolved by the existing prelink graph):"
"$nm_tool" -u "$out/NabuSystemDisk.o" | head -20 || true

# The prelinked kernel exports no stack-probe helper.  A large stack frame makes
# clang call ___chkstk_darwin, which the relink then cannot resolve -- catch it
# here instead of at the end of the pipeline.
if "$nm_tool" -u "$out/NabuSystemDisk.o" | grep -q '__chkstk_darwin'; then
    printf '%s\n' \
        "error: object calls ___chkstk_darwin; a stack frame is too large." \
        "       Move big buffers out of locals (see nabu::buildMetadata)." >&2
    exit 1
fi
