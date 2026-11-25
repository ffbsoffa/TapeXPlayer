#!/bin/bash
# build_sdl2_win.sh — real SDL2 and SDL2_ttf, built from source into the MSYS2 environment.
#
# MSYS2 ships sdl2-compat (a shim that loads SDL3 when the player starts) under the SDL2 name,
# and ffmpeg pulls it in. The player is released with real SDL2, as on macOS: built from source
# over the shim. A later pacman update of sdl2-compat puts the shim back: run this again.
#
# Needs git and the toolchain, cmake, ninja and freetype packages. Used by CI
# (.github/workflows/windows-build.yml) and bootstrap_win.sh.
set -euo pipefail

: "${MINGW_PREFIX:?run this in an MSYS2 UCRT64 shell}"
SDL_TAG=release-2.32.10
TTF_TAG=release-2.24.0

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

git clone --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL     "$WORK/SDL"
git clone --depth 1 --branch "$TTF_TAG" https://github.com/libsdl-org/SDL_ttf "$WORK/SDL_ttf"

cmake -S "$WORK/SDL" -B "$WORK/b-sdl" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$MINGW_PREFIX" -DSDL_SHARED=ON -DSDL_TEST=OFF
cmake --build "$WORK/b-sdl"
cmake --install "$WORK/b-sdl"

cmake -S "$WORK/SDL_ttf" -B "$WORK/b-ttf" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$MINGW_PREFIX" -DCMAKE_PREFIX_PATH="$MINGW_PREFIX" \
    -DBUILD_SHARED_LIBS=ON -DSDL2TTF_SAMPLES=OFF -DSDL2TTF_VENDORED=OFF -DSDL2TTF_HARFBUZZ=OFF
cmake --build "$WORK/b-ttf"
cmake --install "$WORK/b-ttf"

# Fail here, not on a user's computer, if the shim is still what gets linked and bundled
for dll in SDL2.dll SDL2_ttf.dll; do
    test -f "$MINGW_PREFIX/bin/$dll"
    if grep -a -q -i 'SDL3.dll' "$MINGW_PREFIX/bin/$dll"; then
        echo "$dll is the sdl2-compat shim"
        exit 1
    fi
done
echo "SDL2 $SDL_TAG and SDL2_ttf $TTF_TAG installed into $MINGW_PREFIX"
