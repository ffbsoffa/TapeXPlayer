#!/bin/bash
# create_win_installer.sh — Build TapeXPlayer-Setup-<arch>-build<N>.exe via NSIS
#
# Requirements (MSYS2):  pacman -S mingw-w64-x86_64-nsis
# The bundle must exist first:  make && ./bundle_dlls.sh
#
# Usage: ./create_win_installer.sh [arm64|x64] [bundle-dir]
#   arch defaults to MSYSTEM_CARCH (the MSYS2 environment)

set -e

BUILD=$(cat .build_number 2>/dev/null || date +%Y%m%d)

ARCH="${1:-${MSYSTEM_CARCH:-x64}}"
case "$ARCH" in
    aarch64) ARCH="arm64" ;;
    x86_64)  ARCH="x64"   ;;
esac

BUNDLE="${2:-../builds/win-bundle}"

if [ ! -f "$BUNDLE/TapeXPlayer.exe" ]; then
    echo "❌  $BUNDLE/TapeXPlayer.exe not found — first run: make && ./bundle_dlls.sh"
    exit 1
fi

# makensis lives in /mingw64 even when building the ARM64 installer
MAKENSIS="$(command -v makensis || echo /mingw64/bin/makensis)"
if [ ! -x "$MAKENSIS" ]; then
    echo "❌  makensis not found:  pacman -S mingw-w64-x86_64-nsis"
    exit 1
fi

# makensis is a native Windows tool: paths in -D defines must be Windows-style
BUNDLE_WIN="$(cygpath -w "$BUNDLE" 2>/dev/null || echo "$BUNDLE")"

echo "════════════════════════════════════════════════════════════════"
echo "  TapeXPlayer — Windows Installer Builder (NSIS)"
echo "  Build $BUILD  ·  $ARCH  ·  bundle: $BUNDLE"
echo "════════════════════════════════════════════════════════════════"

"$MAKENSIS" -DBUILD_NUMBER="$BUILD" -DARCH="$ARCH" -DBUNDLE_DIR="$BUNDLE_WIN" installer.nsi

echo ""
echo "✅  ../builds/TapeXPlayer-Setup-$ARCH-build$BUILD.exe"
