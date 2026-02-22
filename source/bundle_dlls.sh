#!/bin/bash
# bundle_dlls.sh - Bundle Windows DLL dependencies alongside TapeXPlayer.exe
# Usage: ./bundle_dlls.sh [--mingw-path PATH] [--out-dir PATH] [--exe PATH]
#
# Gathers all required runtime DLLs from MinGW/MSYS2 and copies them to the
# output directory so the final package is fully self-contained.
# DLL search order on Windows puts the .exe directory first, so bundled DLLs
# take priority over any system-installed versions (including Homebrew/vcpkg).

set -e

# ── Defaults ──────────────────────────────────────────────────────────────────
MINGW_PATH="${MSYSTEM_PREFIX:-/mingw64}"
OUT_DIR="../builds/win-bundle"
EXE_PATH="../builds/binaries/TapeXPlayer.exe"

# ── Argument parsing ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mingw-path) MINGW_PATH="$2"; shift 2 ;;
        --out-dir)    OUT_DIR="$2";    shift 2 ;;
        --exe)        EXE_PATH="$2";   shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

MINGW_BIN="$MINGW_PATH/bin"

echo "════════════════════════════════════════════════════════════════"
echo "  TapeXPlayer — Windows DLL Bundler"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "  MinGW bin: $MINGW_BIN"
echo "  Output:    $OUT_DIR"
echo "  Exe:       $EXE_PATH"
echo ""

# ── Validate inputs ───────────────────────────────────────────────────────────
if [ ! -f "$EXE_PATH" ]; then
    echo "❌  TapeXPlayer.exe not found at: $EXE_PATH"
    echo "    Run 'make' on a Windows/MinGW host first."
    exit 1
fi

if [ ! -d "$MINGW_BIN" ]; then
    echo "❌  MinGW bin directory not found: $MINGW_BIN"
    echo "    Set MSYSTEM_PREFIX or pass --mingw-path"
    exit 1
fi

mkdir -p "$OUT_DIR"

# ── Helper: copy one DLL by glob pattern ─────────────────────────────────────
copy_dll() {
    local pattern="$1"
    local required="${2:-yes}"   # "yes" = fatal if missing, "no" = warn only

    # Expand glob
    local found
    found=$(ls "$MINGW_BIN"/$pattern 2>/dev/null | head -1)

    if [ -z "$found" ]; then
        if [ "$required" = "yes" ]; then
            echo "❌  Required DLL not found: $pattern  (searched in $MINGW_BIN)"
            MISSING_REQUIRED=1
        else
            echo "⚠️   Optional DLL not found (skipping): $pattern"
        fi
        return
    fi

    local dll_name
    dll_name=$(basename "$found")

    if [ -f "$OUT_DIR/$dll_name" ]; then
        echo "   ✓  (already copied) $dll_name"
    else
        cp "$found" "$OUT_DIR/$dll_name"
        echo "   📄 $dll_name"
    fi
}

MISSING_REQUIRED=0

# ── FFmpeg (LGPL — must be dynamic) ──────────────────────────────────────────
echo "1️⃣   FFmpeg libraries (LGPL — dynamic link required)..."
copy_dll "avcodec-*.dll"
copy_dll "avformat-*.dll"
copy_dll "avutil-*.dll"
copy_dll "swscale-*.dll"
copy_dll "swresample-*.dll"
# Optional extras sometimes needed by ffmpeg plugins
copy_dll "avfilter-*.dll"  "no"
copy_dll "avdevice-*.dll"  "no"
copy_dll "postproc-*.dll"  "no"
echo ""

# ── SDL2 ──────────────────────────────────────────────────────────────────────
echo "2️⃣   SDL2 libraries..."
copy_dll "SDL2.dll"
copy_dll "SDL2_ttf.dll"
echo ""

# ── PortAudio ─────────────────────────────────────────────────────────────────
echo "3️⃣   PortAudio..."
# MinGW package may be named portaudio or portaudio_x64
copy_dll "portaudio*.dll"
echo ""

# ── OpenSSL ───────────────────────────────────────────────────────────────────
echo "4️⃣   OpenSSL..."
copy_dll "libssl-*.dll"
copy_dll "libcrypto-*.dll"
echo ""

# ── RtMidi ────────────────────────────────────────────────────────────────────
echo "5️⃣   RtMidi (if shared)..."
copy_dll "librtmidi*.dll" "no"
echo ""

# ── MinGW C/C++ runtime ───────────────────────────────────────────────────────
echo "6️⃣   MinGW runtime..."
copy_dll "libstdc++-6.dll"
copy_dll "libgcc_s_seh-1.dll"
copy_dll "libwinpthread-1.dll"
# UCRT runtime (MinGW UCRT variant)
copy_dll "libucrt*.dll" "no"
echo ""

# ── FreeType (used by SDL2_ttf) ───────────────────────────────────────────────
echo "7️⃣   FreeType / HarfBuzz (SDL2_ttf deps)..."
copy_dll "libfreetype-*.dll" "no"
copy_dll "libharfbuzz-*.dll" "no"
copy_dll "libbrotli*.dll"    "no"
copy_dll "libbz2-*.dll"      "no"
copy_dll "libpng*.dll"       "no"
copy_dll "zlib*.dll"         "no"
copy_dll "libzstd*.dll"      "no"
echo ""

# ── Also copy the exe itself into the bundle dir ──────────────────────────────
echo "8️⃣   Copying TapeXPlayer.exe..."
cp "$EXE_PATH" "$OUT_DIR/TapeXPlayer.exe"
echo "   📄 TapeXPlayer.exe"
echo ""

# ── Transitive dependency resolver ───────────────────────────────────────────
# objdump lists all DLL imports for a PE binary.
# For each bundled DLL/exe, we find imports that are NOT system DLLs and NOT
# already in the bundle, then copy them from MinGW — recursively.
# This is the Windows equivalent of the macOS inter-library path patching.

# System DLLs guaranteed on Windows 10+ — never bundle these
_is_system_dll() {
    local name
    name=$(echo "$1" | tr '[:upper:]' '[:lower:]')
    case "$name" in
        kernel32.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll) return 0 ;;
        ntdll.dll|msvcrt.dll|ucrtbase.dll|msvcp*.dll|vcruntime*.dll) return 0 ;;
        ws2_32.dll|winmm.dll|imm32.dll|comctl32.dll|comdlg32.dll)   return 0 ;;
        ole32.dll|oleaut32.dll|shlwapi.dll|rpcrt4.dll|combase.dll)   return 0 ;;
        bcrypt.dll|crypt32.dll|ncrypt.dll|sechost.dll|wintrust.dll)  return 0 ;;
        setupapi.dll|cfgmgr32.dll|version.dll|psapi.dll|dbghelp.dll) return 0 ;;
        d3d11.dll|d3d9.dll|dxgi.dll|opengl32.dll|glu32.dll)          return 0 ;;
        dwmapi.dll|uxtheme.dll|avrt.dll|hid.dll|winspool.drv)        return 0 ;;
        api-ms-win-*.dll|ext-ms-win-*.dll)                            return 0 ;;
    esac
    return 1
}

_copy_transitive() {
    local pe_file="$1"
    if ! command -v objdump &>/dev/null; then return; fi

    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        if _is_system_dll "$dep"; then continue; fi
        if [ -f "$OUT_DIR/$dep" ]; then continue; fi   # already bundled

        # Search in MinGW bin (case-insensitive glob)
        local found
        found=$(find "$MINGW_BIN" -maxdepth 1 -iname "$dep" 2>/dev/null | head -1)
        if [ -n "$found" ]; then
            cp "$found" "$OUT_DIR/$dep"
            echo "   + transitive: $dep"
            _copy_transitive "$OUT_DIR/$dep"   # recurse
        else
            echo "   !! transitive dep not found in MinGW: $dep"
        fi
    done < <(objdump -p "$pe_file" 2>/dev/null | grep "DLL Name" | awk '{print $3}' || true)
}

if command -v objdump &>/dev/null; then
    echo "9️⃣   Resolving transitive DLL dependencies..."
    for pe in "$OUT_DIR"/*.dll "$OUT_DIR/TapeXPlayer.exe"; do
        [ -f "$pe" ] || continue
        _copy_transitive "$pe"
    done
    echo ""
else
    echo "9️⃣   (objdump not found — skipping transitive dep check)"
    echo ""
fi

# ── Integrity check ───────────────────────────────────────────────────────────
if [ "$MISSING_REQUIRED" -ne 0 ]; then
    echo ""
    echo "════════════════════════════════════════════════════════════════"
    echo "❌  One or more required DLLs are missing."
    echo "    Install them via MSYS2:  pacman -S mingw-w64-x86_64-ffmpeg"
    echo "                             pacman -S mingw-w64-x86_64-SDL2"
    echo "════════════════════════════════════════════════════════════════"
    exit 1
fi

# Verify critical DLLs exist in output
CRITICAL=("TapeXPlayer.exe" "SDL2.dll")
VERIFY_FAILED=0
for f in "${CRITICAL[@]}"; do
    if [ ! -f "$OUT_DIR/$f" ]; then
        echo "❌  Critical file missing from output: $f"
        VERIFY_FAILED=1
    fi
done
if [ "$VERIFY_FAILED" -ne 0 ]; then
    exit 1
fi

DLL_COUNT=$(find "$OUT_DIR" -maxdepth 1 -name "*.dll" | wc -l | tr -d ' ')

# ── Final dependency verification ─────────────────────────────────────────────
if command -v objdump &>/dev/null; then
    UNSATISFIED=0
    for pe in "$OUT_DIR"/*.dll "$OUT_DIR/TapeXPlayer.exe"; do
        [ -f "$pe" ] || continue
        while IFS= read -r dep; do
            [ -z "$dep" ] && continue
            if _is_system_dll "$dep"; then continue; fi
            if [ ! -f "$OUT_DIR/$dep" ]; then
                echo "   !! UNSATISFIED: $(basename "$pe") needs $dep"
                UNSATISFIED=$((UNSATISFIED + 1))
            fi
        done < <(objdump -p "$pe" 2>/dev/null | grep "DLL Name" | awk '{print $3}' || true)
    done
    if [ "$UNSATISFIED" -gt 0 ]; then
        echo ""
        echo "   !! $UNSATISFIED unsatisfied DLL dependencies remain."
        echo "      Install missing packages via: pacman -S <package>"
    else
        echo "   All DLL imports satisfied."
    fi
fi

echo "════════════════════════════════════════════════════════════════"
echo "✅  DLL bundling complete!"
echo "    Output directory: $OUT_DIR"
echo "    DLLs copied: $DLL_COUNT"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "All bundled DLLs will take priority over any system-installed"
echo "versions (Windows DLL search order: exe dir > system dirs)."
echo ""
