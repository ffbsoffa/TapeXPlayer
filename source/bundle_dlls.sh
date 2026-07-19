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
# MinGW names it libportaudio.dll (not portaudio.dll)
copy_dll "libportaudio*.dll"
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

# ── FFmpeg CLI binaries ───────────────────────────────────────────────────────
# The proxy converter shells out to ffmpeg/ffprobe (FSTPProxyConverter.cpp runs them
# by bare name). Without these in the bundle a fresh machine with no ffmpeg on PATH
# just stalls the proxy at 0%. They link the same libav* DLLs already bundled above.
echo "8️⃣   FFmpeg CLI tools (proxy converter runs ffmpeg/ffprobe)..."
for _tool in ffmpeg ffprobe; do
    if [ -f "$MINGW_BIN/$_tool.exe" ]; then
        cp "$MINGW_BIN/$_tool.exe" "$OUT_DIR/$_tool.exe"
        echo "   🔧 $_tool.exe"
    else
        echo "❌  $_tool.exe not found in $MINGW_BIN — proxy conversion can't run on a bare machine"
        MISSING_REQUIRED=1
    fi
done
echo ""

# ── Also copy the exe itself into the bundle dir ──────────────────────────────
echo "9️⃣   Copying TapeXPlayer.exe..."
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
        # GDI/graphics system DLLs
        msimg32.dll|gdiplus.dll|d2d1.dll|dwrite.dll)                 return 0 ;;
        # Network/DNS system DLLs
        dnsapi.dll|iphlpapi.dll|wsock32.dll|mswsock.dll|winnsi.dll)  return 0 ;;
        # User/profile/security system DLLs
        userenv.dll|profapi.dll|wtsapi32.dll|usp10.dll)               return 0 ;;
        # Video capture / multimedia system DLLs
        avicap32.dll|msvfw32.dll|quartz.dll)                          return 0 ;;
    esac
    return 1
}

echo "🔟   Resolving transitive DLL dependencies..."
if command -v ldd &>/dev/null; then
    # ldd uses the Windows loader — resolves ALL transitive deps in one shot,
    # much faster than walking objdump output recursively on MSYS2.
    # Walk the app AND the ffmpeg/ffprobe binaries so their extra codec/filter
    # DLLs get pulled in too (most overlap with the app's libav* deps already).
    LDD_ADDED=0
    for _bin in "$EXE_PATH" "$OUT_DIR/ffmpeg.exe" "$OUT_DIR/ffprobe.exe"; do
        [ -f "$_bin" ] || continue
        while IFS= read -r dll_path; do
            [ -z "$dll_path" ] || [ ! -f "$dll_path" ] && continue
            dll_name=$(basename "$dll_path")
            _is_system_dll "$dll_name" && continue
            if [ ! -f "$OUT_DIR/$dll_name" ]; then
                cp "$dll_path" "$OUT_DIR/$dll_name"
                echo "   + transitive: $dll_name"
                LDD_ADDED=$((LDD_ADDED + 1))
            fi
        done < <(ldd "$_bin" 2>/dev/null \
                 | awk '$3 ~ /\// {print $3}' \
                 | grep -i "mingw" \
                 | sort -u)
    done
    echo "   Added $LDD_ADDED additional DLLs via ldd"
elif command -v objdump &>/dev/null; then
    # Fallback: single objdump pass (no recursion, no find-per-dep)
    echo "   (ldd not found, using objdump — only direct deps)"
    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        _is_system_dll "$dep" && continue
        [ -f "$OUT_DIR/$dep" ] && continue
        src=$(ls "$MINGW_BIN/$dep" 2>/dev/null | head -1)
        [ -z "$src" ] && src=$(ls "$MINGW_BIN/"*"$dep" 2>/dev/null | head -1)
        if [ -n "$src" ]; then
            cp "$src" "$OUT_DIR/$dep"
            echo "   + $dep"
        fi
    done < <(objdump -p "$OUT_DIR/TapeXPlayer.exe" 2>/dev/null \
             | grep "DLL Name" | awk '{print $3}' || true)
else
    echo "   (neither ldd nor objdump found — skipping)"
fi
echo ""

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

# ── Final dependency verification (ldd-based, fast) ──────────────────────────
# Check that all DLLs needed by TapeXPlayer.exe are present in OUT_DIR.
# Uses ldd on the ORIGINAL exe so results are not affected by OUT_DIR contents.
if command -v ldd &>/dev/null; then
    UNSATISFIED=0
    while IFS= read -r dep_name; do
        [ -z "$dep_name" ] && continue
        _is_system_dll "$dep_name" && continue
        if [ ! -f "$OUT_DIR/$dep_name" ]; then
            echo "   !! UNSATISFIED: $dep_name"
            UNSATISFIED=$((UNSATISFIED + 1))
        fi
    done < <(ldd "$EXE_PATH" 2>/dev/null \
             | awk '{print $1}' \
             | grep -iv "^ntdll\|^kernel\|^user32\|^msvcrt\|not$" \
             | grep -i "\.dll$" \
             | sort -u)
    if [ "$UNSATISFIED" -gt 0 ]; then
        echo "   !! $UNSATISFIED unsatisfied DLL dependencies."
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
