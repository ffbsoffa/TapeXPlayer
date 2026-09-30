#!/bin/bash
# build-macos-x86_64-deps.sh — cross-build every x86_64 library TapeXPlayer links, from source,
# on an Apple Silicon Mac, into one prefix (default /usr/local — what `make bundle-universal`
# uses for the x86_64 slice).
#
# Why: Homebrew dropped Intel macOS, its installer refuses to run under x86_64
# ("Homebrew on macOS is only supported on Apple Silicon processors!"), so the old
# "second Homebrew in /usr/local via Rosetta" route is gone.
#
# Needs on the host (arm64 Homebrew is fine): Xcode CLT, cmake, ninja, meson, nasm, pkg-config.
# FFmpeg is a decode-only build with --disable-autodetect, so nothing from the arm64
# Homebrew leaks in; dav1d gives it AV1. Re-running skips what is already installed.
#
#   Usage: build-macos-x86_64-deps.sh [prefix]

set -euo pipefail

PREFIX="${1:-/usr/local}"
MIN_MACOS=13.0

FFMPEG_VER=9.0.2
DAV1D_VER=1.5.4
OPENSSL_VER=3.5.9
PORTAUDIO_TAG=v19.7.0
RTMIDI_TAG=6.0.0
FREETYPE_TAG=VER-2-14-3
SDL2_TAG=release-2.32.10
SDL2_TTF_TAG=release-2.24.0

WORK="$(mktemp -d)"
JOBS="$(sysctl -n hw.ncpu)"
export MACOSX_DEPLOYMENT_TARGET=$MIN_MACOS
# Only our own x86_64 .pc files — never the arm64 Homebrew ones.
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH=""

mkdir -p "$PREFIX/lib" "$PREFIX/include" "$PREFIX/lib/pkgconfig"

have() { [ -f "$PREFIX/lib/$1" ] && lipo -archs "$PREFIX/lib/$1" 2>/dev/null | grep -q x86_64; }

CMAKE_X86=(
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_OSX_ARCHITECTURES=x86_64
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_MACOS
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DCMAKE_INSTALL_NAME_DIR="$PREFIX/lib"
    -DCMAKE_PREFIX_PATH="$PREFIX"
    -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew
)

cmake_build() {   # $1=src dir, rest = extra cmake args
    local src="$1"; shift
    cmake -S "$src" -B "$src/build-x86_64" "${CMAKE_X86[@]}" "$@"
    cmake --build "$src/build-x86_64" -j"$JOBS"
    cmake --install "$src/build-x86_64"
}

clone() { git clone -q --depth 1 --branch "$2" "https://github.com/$1.git" "$WORK/$3"; }

# ── OpenSSL ──────────────────────────────────────────────────────────────────
if ! have libssl.3.dylib; then
    echo "── OpenSSL $OPENSSL_VER"
    curl -fsSL "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz" | tar xz -C "$WORK"
    (cd "$WORK/openssl-$OPENSSL_VER" &&
        ./Configure darwin64-x86_64-cc shared no-tests no-docs --prefix="$PREFIX" --libdir=lib \
            -mmacosx-version-min=$MIN_MACOS &&
        make -j"$JOBS" >/dev/null && make install_sw >/dev/null)
fi

# ── PortAudio ────────────────────────────────────────────────────────────────
if ! have libportaudio.dylib; then
    echo "── PortAudio $PORTAUDIO_TAG"
    clone PortAudio/portaudio "$PORTAUDIO_TAG" portaudio
    cmake_build "$WORK/portaudio" -DPA_BUILD_SHARED=ON -DPA_BUILD_STATIC=OFF
fi

# ── RtMidi ───────────────────────────────────────────────────────────────────
if ! have librtmidi.dylib; then
    echo "── RtMidi $RTMIDI_TAG"
    clone thestk/rtmidi "$RTMIDI_TAG" rtmidi
    cmake_build "$WORK/rtmidi" -DBUILD_SHARED_LIBS=ON -DRTMIDI_BUILD_TESTING=OFF -DRTMIDI_API_JACK=OFF
fi

# ── FreeType (for SDL2_ttf) ──────────────────────────────────────────────────
if ! have libfreetype.dylib; then
    echo "── FreeType $FREETYPE_TAG"
    clone freetype/freetype "$FREETYPE_TAG" freetype
    cmake_build "$WORK/freetype" -DBUILD_SHARED_LIBS=ON \
        -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
fi

# ── SDL2 + SDL2_ttf (real SDL2 — Homebrew's sdl2 is the sdl2-compat SDL3 shim) ─
if ! have libSDL2-2.0.0.dylib; then
    echo "── SDL2 $SDL2_TAG"
    clone libsdl-org/SDL "$SDL2_TAG" SDL
    cmake_build "$WORK/SDL" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
fi
if ! have libSDL2_ttf-2.0.0.dylib; then
    echo "── SDL2_ttf $SDL2_TTF_TAG"
    clone libsdl-org/SDL_ttf "$SDL2_TTF_TAG" SDL_ttf
    cmake_build "$WORK/SDL_ttf" -DBUILD_SHARED_LIBS=ON \
        -DSDL2TTF_SAMPLES=OFF -DSDL2TTF_VENDORED=OFF -DSDL2TTF_HARFBUZZ=OFF
fi

# ── dav1d (AV1 decoder for FFmpeg) ───────────────────────────────────────────
if ! have libdav1d.dylib; then
    echo "── dav1d $DAV1D_VER"
    clone videolan/dav1d "$DAV1D_VER" dav1d
    cat > "$WORK/x86_64-darwin.ini" <<EOF
[binaries]
c = ['clang', '-arch', 'x86_64', '-mmacosx-version-min=$MIN_MACOS']
cpp = ['clang++', '-arch', 'x86_64', '-mmacosx-version-min=$MIN_MACOS']
ar = 'ar'
strip = 'strip'
nasm = 'nasm'
[host_machine]
system = 'darwin'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
    meson setup "$WORK/dav1d/build" "$WORK/dav1d" --cross-file "$WORK/x86_64-darwin.ini" \
        --prefix="$PREFIX" --libdir=lib --buildtype=release --default-library=shared \
        -Denable_tools=false -Denable_tests=false
    ninja -C "$WORK/dav1d/build" install
fi

# ── FFmpeg (decode-only, nothing auto-detected from the host) ────────────────
if ! have libavcodec.dylib; then
    echo "── FFmpeg $FFMPEG_VER"
    curl -fsSL "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz" | tar xJ -C "$WORK"
    (cd "$WORK/ffmpeg-$FFMPEG_VER" &&
        ./configure --prefix="$PREFIX" \
            --enable-cross-compile --arch=x86_64 --target-os=darwin \
            --cc="clang -arch x86_64" --cxx="clang++ -arch x86_64" \
            --extra-cflags="-mmacosx-version-min=$MIN_MACOS" \
            --extra-ldflags="-mmacosx-version-min=$MIN_MACOS -L$PREFIX/lib" \
            --extra-libs=-liconv \
            --pkg-config=pkg-config \
            --enable-shared --disable-static \
            --disable-programs --disable-doc --disable-network \
            --disable-autodetect \
            --enable-videotoolbox --enable-audiotoolbox \
            --enable-zlib --enable-bzlib --enable-iconv \
            --enable-libdav1d \
            --install-name-dir="$PREFIX/lib" &&
        make -j"$JOBS" >/dev/null && make install >/dev/null)
fi

# ── Every dylib must be x86_64 ───────────────────────────────────────────────
bad=0
for lib in "$PREFIX"/lib/*.dylib; do
    [ -L "$lib" ] && continue
    lipo -archs "$lib" | grep -q x86_64 || { echo "❌ not x86_64: $lib"; bad=1; }
    if otool -L "$lib" | grep -q /opt/homebrew; then echo "❌ links arm64 Homebrew: $lib"; bad=1; fi
done
[ "$bad" -eq 0 ] || exit 1
echo "✅ x86_64 deps in $PREFIX"
rm -rf "$WORK"
