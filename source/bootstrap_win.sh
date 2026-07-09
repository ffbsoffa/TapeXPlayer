#!/bin/bash
# bootstrap_win.sh — one-shot setup for building TapeXPlayer (x64) on Windows.
#
# Run this INSIDE the "MSYS2 MINGW64" shell (NOT the plain "MSYS2" / "UCRT64" shell).
# It installs the MinGW-w64 toolchain + all runtime dependencies, then builds
# TapeXPlayer.exe and bundles its DLLs.
#
#   Usage (from anywhere inside MSYS2 MINGW64):
#     curl -fsSL https://raw.githubusercontent.com/ffbsoffa/TapeXPlayer/main/source/bootstrap_win.sh -o bootstrap_win.sh
#     bash bootstrap_win.sh
#
#   Or, if you already cloned the repo, just run it from source/:
#     ./bootstrap_win.sh
#
# Env overrides:
#   REPO_URL   git URL to clone (default: ffbsoffa/TapeXPlayer)
#   SKIP_CLONE set to 1 when running from an existing checkout
#   SKIP_BUILD set to 1 to only install deps, not build

set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/ffbsoffa/TapeXPlayer}"

# ── Sanity: must be the MINGW64 environment ───────────────────────────────────
if [ "${MSYSTEM:-}" != "MINGW64" ]; then
    echo "❌  Wrong shell: MSYSTEM='${MSYSTEM:-unset}'"
    echo "    Open the 'MSYS2 MINGW64' shell (blue icon) and re-run this script."
    echo "    The makefile links against /mingw64 libraries and needs this environment."
    exit 1
fi

echo "════════════════════════════════════════════════════════════════"
echo "  TapeXPlayer — Windows build bootstrap (MSYS2 / MINGW64, x86_64)"
echo "════════════════════════════════════════════════════════════════"
echo ""

# ── 1. Update the package database & core packages ────────────────────────────
# Note: a first `pacman -Syu` may ask to close the terminal. If so, reopen the
# MINGW64 shell and run this script again — it is safe to re-run (idempotent).
echo "1️⃣   Updating MSYS2 package database..."
pacman -Syu --noconfirm

# ── 2. Install toolchain + dependencies ───────────────────────────────────────
# Package list derived from the makefile's LIBS:
#   SDL2, SDL2_ttf, portaudio, ffmpeg (avformat/avcodec/avfilter/avutil/swscale),
#   openssl (ssl/crypto), rtmidi.  Plus g++ (c++23), make, windres, nsis, zip, git.
echo ""
echo "2️⃣   Installing toolchain and runtime dependencies..."
pacman -S --needed --noconfirm \
    mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-gcc \
    make \
    mingw-w64-x86_64-SDL2 \
    mingw-w64-x86_64-SDL2_ttf \
    mingw-w64-x86_64-portaudio \
    mingw-w64-x86_64-ffmpeg \
    mingw-w64-x86_64-openssl \
    mingw-w64-x86_64-rtmidi \
    mingw-w64-x86_64-nsis \
    zip git

echo ""
echo "   Toolchain versions:"
echo "     $(g++ --version | head -1)"
echo "     $(make --version | head -1)"

# ── 3. Get the source ─────────────────────────────────────────────────────────
if [ "${SKIP_CLONE:-0}" != "1" ]; then
    if [ -f "makefile" ] && [ -d "modules/FSTPMainModule" ]; then
        echo ""
        echo "3️⃣   Already inside a TapeXPlayer/source checkout — skipping clone."
    else
        echo ""
        echo "3️⃣   Cloning $REPO_URL ..."
        git clone "$REPO_URL"
        cd TapeXPlayer/source
    fi
fi

# ── 4. Build ──────────────────────────────────────────────────────────────────
if [ "${SKIP_BUILD:-0}" = "1" ]; then
    echo ""
    echo "✅  Dependencies installed. Skipping build (SKIP_BUILD=1)."
    echo "    To build:  make && ./bundle_dlls.sh"
    exit 0
fi

echo ""
echo "4️⃣   Building TapeXPlayer.exe ..."
make -j"$(nproc)"

echo ""
echo "5️⃣   Bundling runtime DLLs ..."
./bundle_dlls.sh

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "✅  Done!"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "  Binary:  ../builds/binaries/TapeXPlayer.exe"
echo "  Bundle:  ../builds/win-bundle/  (exe + all DLLs, self-contained)"
echo ""
echo "  Next (optional):"
echo "    ./create_win_release.sh    # -> ../builds/TapeXPlayer-*-win.zip"
echo "    ./create_win_installer.sh  # -> NSIS installer .exe"
echo ""
