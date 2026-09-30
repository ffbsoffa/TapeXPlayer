#!/bin/bash
# check_deps.sh — run by `make` before compiling (or on its own: `make check`).
#
# Checks the compiler and every library the makefile links against. When something is
# missing it prints ONE install command for this system instead of letting the build die
# on "SDL2/SDL.h: No such file or directory" twenty files in. Skip with SKIP_CHECK=1.
#
# Env (set by the makefile):
#   CHECK_PLATFORM  macos | linux | windows
#   DEPS_PREFIX     where headers live on macOS (Homebrew prefix) and Windows (/mingw64)
#   CXX             compiler the makefile uses

set -u

PLATFORM="${CHECK_PLATFORM:?CHECK_PLATFORM not set — run this through make check}"
PREFIX="${DEPS_PREFIX:-}"
CXX="${CXX:-g++}"

if [ -t 1 ]; then
    OK=$'\e[32m✓\e[0m'; BAD=$'\e[31m✗\e[0m'; OPT=$'\e[33m–\e[0m'; DIM=$'\e[2m'; RST=$'\e[0m'
else
    OK='✓'; BAD='✗'; OPT='–'; DIM=''; RST=''
fi

MISSING_PKGS=()   # package names for the combined install command
PROBLEMS=()       # problems no package install fixes (wrong shell, old Xcode, ...)

# ── Package manager and package names ────────────────────────────────────────
case "$PLATFORM" in
    macos)   PM=brew ;;
    windows) PM=msys2 ;;
    linux)
        if   command -v apt-get >/dev/null 2>&1; then PM=apt
        elif command -v dnf     >/dev/null 2>&1; then PM=dnf
        elif command -v pacman  >/dev/null 2>&1; then PM=pacman
        else PM=unknown; fi ;;
esac

MW="${MINGW_PACKAGE_PREFIX:-mingw-w64-x86_64}"

# pkg_for <dep> → package name(s) for this package manager
pkg_for() {
    case "$PM:$1" in
        apt:compiler)    echo "build-essential" ;;
        apt:pkgconfig)   echo "pkg-config" ;;
        apt:sdl2)        echo "libsdl2-dev" ;;
        apt:sdl2_ttf)    echo "libsdl2-ttf-dev" ;;
        apt:portaudio)   echo "portaudio19-dev" ;;
        apt:ffmpeg)      echo "libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev libswscale-dev libswresample-dev" ;;
        apt:rtmidi)      echo "librtmidi-dev" ;;
        apt:openssl)     echo "libssl-dev" ;;
        apt:gtk)         echo "libgtk-3-dev" ;;

        dnf:compiler)    echo "gcc-c++ make" ;;
        dnf:pkgconfig)   echo "pkgconf-pkg-config" ;;
        dnf:sdl2)        echo "SDL2-devel" ;;
        dnf:sdl2_ttf)    echo "SDL2_ttf-devel" ;;
        dnf:portaudio)   echo "portaudio-devel" ;;
        dnf:ffmpeg)      echo "ffmpeg-free-devel" ;;
        dnf:rtmidi)      echo "rtmidi-devel" ;;
        dnf:openssl)     echo "openssl-devel" ;;
        dnf:gtk)         echo "gtk3-devel" ;;

        pacman:compiler) echo "base-devel" ;;
        pacman:pkgconfig) echo "pkgconf" ;;
        pacman:sdl2)     echo "sdl2" ;;
        pacman:sdl2_ttf) echo "sdl2_ttf" ;;
        pacman:gtk)      echo "gtk3" ;;
        pacman:*)        echo "$1" ;;          # portaudio ffmpeg rtmidi openssl

        brew:openssl)    echo "openssl@3" ;;
        brew:compiler)   echo "" ;;            # comes with Xcode, handled separately
        brew:*)          echo "$1" ;;          # sdl2 sdl2_ttf portaudio ffmpeg rtmidi lua

        msys2:compiler)  echo "$MW-toolchain make" ;;
        msys2:sdl2)      echo "$MW-SDL2" ;;
        msys2:sdl2_ttf)  echo "$MW-SDL2_ttf" ;;
        msys2:*)         echo "$MW-$1" ;;      # portaudio ffmpeg rtmidi openssl

        *)               echo "" ;;
    esac
}

install_cmd() {
    case "$PM" in
        apt)    echo "sudo apt install" ;;
        dnf)    echo "sudo dnf install" ;;
        pacman) echo "sudo pacman -S --needed" ;;
        brew)   echo "brew install" ;;
        msys2)  echo "pacman -S --needed" ;;
    esac
}

pass() { printf "  %s %-12s %s\n" "$OK" "$1" "${DIM}${2:-}${RST}"; }
fail() {   # fail <label> <dep id> [note]
    local pkgs; pkgs="$(pkg_for "$2")"
    if [ -n "$pkgs" ]; then
        printf "  %s %-12s not found → %s %s\n" "$BAD" "$1" "$(install_cmd)" "$pkgs"
        # shellcheck disable=SC2206
        MISSING_PKGS+=($pkgs)
    else
        printf "  %s %-12s not found → install its development package\n" "$BAD" "$1"
        PROBLEMS+=("$1: install the development package (headers + pkg-config file)")
    fi
    [ -n "${3:-}" ] && printf "      %s\n" "$3"
    return 0
}

# version_of <pkg-config module> → version or empty (display only)
version_of() {
    if [ "$PLATFORM" = linux ]; then
        pkg-config --modversion "$1" 2>/dev/null | head -1
    elif command -v pkg-config >/dev/null 2>&1; then
        PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" pkg-config --modversion "$1" 2>/dev/null | head -1
    fi
}

# has_header <relative header> → searches the prefix (macOS/Windows)
has_header() {
    [ -f "$PREFIX/include/$1" ] && return 0
    # openssl@3 is keg-only in Homebrew (the makefile adds its opt/ path the same way)
    [ "$PLATFORM" = macos ] && [ -f "$PREFIX/opt/openssl@3/include/$1" ] && return 0
    return 1
}

# lib <label> <dep id> <pkg-config modules> <header>
lib() {
    local label="$1" dep="$2" mods="$3" header="$4" m
    if [ "$PLATFORM" = linux ]; then
        for m in $mods; do
            pkg-config --exists "$m" 2>/dev/null || { fail "$label" "$dep"; return; }
        done
    else
        has_header "$header" || { fail "$label" "$dep"; return; }
    fi
    local ver; ver="$(version_of "${mods%% *}")"
    [ -n "$ver" ] && [ "$dep" = ffmpeg ] && ver="libavcodec $ver"
    # Homebrew's sdl2 is sdl2-compat (SDL2 API over SDL3): builds and runs locally,
    # but a bundle made from it needs SDL3 on the target Mac (CI builds real SDL2).
    if [ "$dep" = sdl2 ] && [ "$PLATFORM" = macos ] &&
       strings -a "$PREFIX/lib/libSDL2-2.0.0.dylib" 2>/dev/null | grep -qi 'sdl2-compat'; then
        ver="$ver (sdl2-compat: fine for local builds)"
    fi
    pass "$label" "$ver"
}

echo "Checking build dependencies ($PLATFORM)..."

# ── Toolchain ────────────────────────────────────────────────────────────────
case "$PLATFORM" in
    macos)
        if ! xcode-select -p >/dev/null 2>&1; then
            printf "  %s %-12s not found → xcode-select --install\n" "$BAD" "Xcode tools"
            PROBLEMS+=("Install the Xcode command line tools: xcode-select --install")
        fi
        if ! command -v swiftc >/dev/null 2>&1; then
            printf "  %s %-12s not found → install Xcode 16+\n" "$BAD" "swiftc"
            PROBLEMS+=("swiftc is missing: install Xcode 16+ from the App Store")
        fi
        if ! command -v brew >/dev/null 2>&1; then
            printf "  %s %-12s not found → https://brew.sh\n" "$BAD" "Homebrew"
            PROBLEMS+=("Install Homebrew from https://brew.sh")
        fi
        ;;
    windows)
        case "${MSYSTEM:-}" in
            MINGW64|CLANGARM64) ;;
            *)
                printf "  %s %-12s MSYSTEM=%s → open the \"MSYS2 MINGW64\" shell\n" "$BAD" "MSYS2 shell" "${MSYSTEM:-unset}"
                PROBLEMS+=("Wrong MSYS2 shell (${MSYSTEM:-unset}). Build from the \"MSYS2 MINGW64\" shell — the makefile links against /mingw64.")
                ;;
        esac
        command -v windres >/dev/null 2>&1 || fail "windres" compiler
        ;;
    linux)
        if ! command -v pkg-config >/dev/null 2>&1; then
            fail "pkg-config" pkgconfig "Needed to find the libraries — install it and re-run make."
            echo ""
            echo "Install it first:"
            echo "  $(install_cmd) ${MISSING_PKGS[*]}"
            exit 1
        fi
        ;;
esac

if ! command -v "$CXX" >/dev/null 2>&1; then
    if [ "$PLATFORM" = macos ]; then
        printf "  %s %-12s %s not found → xcode-select --install\n" "$BAD" "C++23" "$CXX"
        PROBLEMS+=("No C++ compiler ($CXX): install the Xcode command line tools: xcode-select --install")
    else
        fail "$CXX" compiler
    fi
elif ! echo 'int main(){}' | "$CXX" -std=c++23 -x c++ -fsyntax-only - >/dev/null 2>&1; then
    cxx_ver="$("$CXX" --version 2>/dev/null | head -1)"
    printf "  %s %-12s %s — no -std=c++23\n" "$BAD" "C++23" "$cxx_ver"
    case "$PLATFORM" in
        macos)   PROBLEMS+=("Apple clang is too old for C++23: install Xcode 16+ and select it: sudo xcode-select -s /Applications/Xcode.app") ;;
        linux)   PROBLEMS+=("GCC 11+ is needed for C++23. Install a newer one (e.g. sudo apt install g++-13) and build with: make CXX=g++-13") ;;
        windows) PROBLEMS+=("The MinGW compiler is too old for C++23: pacman -Syu") ;;
    esac
else
    pass "C++23" "$("$CXX" --version 2>/dev/null | head -1)"
fi

# ── Libraries ────────────────────────────────────────────────────────────────
lib "FFmpeg"    ffmpeg    "libavcodec libavformat libavfilter libavutil libswscale libswresample" "libavcodec/avcodec.h"
lib "SDL2"      sdl2      "sdl2"           "SDL2/SDL.h"
lib "SDL2_ttf"  sdl2_ttf  "SDL2_ttf"       "SDL2/SDL_ttf.h"
lib "PortAudio" portaudio "portaudio-2.0"  "portaudio.h"
lib "RtMidi"    rtmidi    "rtmidi"         "rtmidi/RtMidi.h"
lib "OpenSSL"   openssl   "openssl"        "openssl/ssl.h"
[ "$PLATFORM" = linux ] && lib "GTK 3" gtk "gtk+-3.0" ""

if [ "$PLATFORM" = macos ]; then
    if pkg-config --exists lua 2>/dev/null; then
        pass "Lua" "$(pkg-config --modversion lua 2>/dev/null) (extensions on)"
    else
        printf "  %s %-12s %s\n" "$OPT" "Lua" "${DIM}optional, extensions off → brew install lua${RST}"
    fi
fi

# ── Verdict ──────────────────────────────────────────────────────────────────
if [ ${#MISSING_PKGS[@]} -eq 0 ] && [ ${#PROBLEMS[@]} -eq 0 ]; then
    echo "  All dependencies found."
    echo ""
    exit 0
fi

echo ""
echo "Can't build yet."
if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
    # de-duplicate, keep order
    uniq_pkgs=()
    for p in "${MISSING_PKGS[@]}"; do
        case " ${uniq_pkgs[*]:-} " in *" $p "*) ;; *) uniq_pkgs+=("$p") ;; esac
    done
    echo "Install the missing packages:"
    echo ""
    echo "  $(install_cmd) ${uniq_pkgs[*]}"
    echo ""
fi
for p in "${PROBLEMS[@]:-}"; do [ -n "$p" ] && echo "• $p"; done
[ ${#PROBLEMS[@]} -gt 0 ] && echo ""
echo "Then run make again. (SKIP_CHECK=1 skips this check.)"
exit 1
