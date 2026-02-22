#!/bin/bash
# create_win_release.sh - Create Windows release ZIP for GitHub
# Usage: ./create_win_release.sh [build] [codename]
#
# Can be run on Windows (MSYS2/Git Bash) or on macOS/Linux for cross-release prep.
# Expects DLLs to already be bundled via bundle_dlls.sh (or the win-bundle dir to exist).

set -e

# ── Version info ──────────────────────────────────────────────────────────────
BUILD="${1:-}"
CODENAME="${2:-}"

# Try to read from makefile's .build_number if not passed
if [ -z "$BUILD" ]; then
    if [ -f ".build_number" ]; then
        BUILD=$(cat ".build_number")
    else
        BUILD=$(date +"%Y%m%d")
    fi
fi

if [ -z "$CODENAME" ]; then
    CODENAME="Albatross"
fi

RELEASE_NAME="TapeXPlayer-${CODENAME}-build${BUILD}-win"
BUNDLE_DIR="../builds/win-bundle"
RELEASE_DIR="../builds/win-release"
RELEASE_CONTENT="$RELEASE_DIR/$RELEASE_NAME"
RELEASE_ZIP="../builds/${RELEASE_NAME}.zip"

echo "════════════════════════════════════════════════════════════════"
echo "  TapeXPlayer — Windows Release Builder"
echo "  Release: $RELEASE_NAME"
echo "════════════════════════════════════════════════════════════════"
echo ""

# ── Validate bundle ───────────────────────────────────────────────────────────
if [ ! -f "$BUNDLE_DIR/TapeXPlayer.exe" ]; then
    echo "❌  TapeXPlayer.exe not found in $BUNDLE_DIR"
    echo ""
    echo "    On Windows/MSYS2, build first:"
    echo "      make"
    echo "      ./bundle_dlls.sh"
    echo ""
    exit 1
fi

# Check critical DLLs
REQUIRED_DLLS=("SDL2.dll")
for dll in "${REQUIRED_DLLS[@]}"; do
    if [ ! -f "$BUNDLE_DIR/$dll" ]; then
        echo "❌  Required DLL missing from bundle: $dll"
        echo "    Run ./bundle_dlls.sh first."
        exit 1
    fi
done

echo "1️⃣   Preparing release directory..."
rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_CONTENT"

# ── Copy exe + DLLs ───────────────────────────────────────────────────────────
echo "2️⃣   Copying TapeXPlayer.exe and DLLs..."
cp "$BUNDLE_DIR/TapeXPlayer.exe" "$RELEASE_CONTENT/"
find "$BUNDLE_DIR" -maxdepth 1 -name "*.dll" -exec cp {} "$RELEASE_CONTENT/" \;
DLL_COUNT=$(find "$RELEASE_CONTENT" -maxdepth 1 -name "*.dll" | wc -l | tr -d ' ')
echo "   Copied $DLL_COUNT DLLs"

# ── Copy installer scripts ────────────────────────────────────────────────────
echo "3️⃣   Copying installer scripts..."
if [ -f "install.ps1" ]; then
    cp "install.ps1" "$RELEASE_CONTENT/"
    echo "   ✓ install.ps1"
fi

# ── Create uninstall.ps1 ──────────────────────────────────────────────────────
cat > "$RELEASE_CONTENT/uninstall.ps1" << 'PSEOF'
# TapeXPlayer Uninstaller for Windows
# Run with: powershell -ExecutionPolicy Bypass -File uninstall.ps1

$ErrorActionPreference = "Stop"

$installDir  = "$env:PROGRAMFILES\TapeXPlayer"
$localDir    = "$env:LOCALAPPDATA\TapeXPlayer"          # install dir (non-admin) + cache
$roamingDir  = "$env:APPDATA\TapeXPlayer"               # settings + memory locations
$startMenu   = "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\TapeXPlayer"
$desktop     = [Environment]::GetFolderPath("Desktop")
$shortcut    = "$desktop\TapeXPlayer.lnk"

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "   TapeXPlayer Uninstaller" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

$found = $false
if (Test-Path $installDir) { $found = $true; Write-Host "  Program files:  $installDir" }
if (Test-Path $localDir)   { $found = $true; Write-Host "  Local data:     $localDir" }
if (Test-Path $roamingDir) { $found = $true; Write-Host "  Settings/data:  $roamingDir" }

if (-not $found) {
    Write-Host "TapeXPlayer is not installed." -ForegroundColor Yellow
    Read-Host "Press Enter to exit"
    exit 0
}

$confirm = Read-Host "Remove TapeXPlayer completely? (y/n)"
if ($confirm -notmatch '^[Yy]') {
    Write-Host "Uninstallation cancelled."
    exit 0
}

# Remove program files
if (Test-Path $installDir) {
    Remove-Item -Recurse -Force $installDir
    Write-Host "  Removed: $installDir" -ForegroundColor Green
}
if (Test-Path $localDir) {
    Remove-Item -Recurse -Force $localDir
    Write-Host "  Removed: $localDir" -ForegroundColor Green
}

# Remove user data (settings + memory locations)
if (Test-Path $roamingDir) {
    Remove-Item -Recurse -Force $roamingDir
    Write-Host "  Removed: $roamingDir" -ForegroundColor Green
}

# Remove shortcuts
if (Test-Path $shortcut)  { Remove-Item -Force $shortcut;            Write-Host "  Removed desktop shortcut" -ForegroundColor Green }
if (Test-Path $startMenu) { Remove-Item -Recurse -Force $startMenu;  Write-Host "  Removed Start Menu entry" -ForegroundColor Green }

# Remove registry entry (Apps & Features)
$regPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\TapeXPlayer"
if (Test-Path $regPath) {
    Remove-Item -Path $regPath -Force
    Write-Host "  Removed Add/Remove Programs entry" -ForegroundColor Green
}
$regPathUser = "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\TapeXPlayer"
if (Test-Path $regPathUser) {
    Remove-Item -Path $regPathUser -Force
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  TapeXPlayer removed successfully." -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Read-Host "Press Enter to exit"
PSEOF

echo "   ✓ uninstall.ps1"

# ── Copy LICENSES ─────────────────────────────────────────────────────────────
echo "4️⃣   Copying LICENSES_THIRD_PARTY.txt..."
if [ -f "LICENSES_THIRD_PARTY.txt" ]; then
    cp "LICENSES_THIRD_PARTY.txt" "$RELEASE_CONTENT/"
else
    echo "⚠️   LICENSES_THIRD_PARTY.txt not found — skipping (create it!)"
fi

# ── Generate README.txt ───────────────────────────────────────────────────────
echo "5️⃣   Generating README.txt..."
cat > "$RELEASE_CONTENT/README.txt" << EOF
================================================================================
                    TapeXPlayer "${CODENAME}"
                            Build ${BUILD}
                        Windows Edition
================================================================================

Professional Video Player for Windows
Windows 10 or later required (x86_64)

================================================================================
QUICK INSTALL / БЫСТРАЯ УСТАНОВКА
================================================================================

Option 1 — PowerShell installer (recommended):
  Right-click install.ps1 → "Run with PowerShell"
  Or in PowerShell:
    powershell -ExecutionPolicy Bypass -File install.ps1

Option 2 — Portable (no installation):
  Run TapeXPlayer.exe directly from this folder.
  All required DLLs are included — no additional software needed.

Option 3 — One-line install from web (PowerShell):
  iwr -useb https://raw.githubusercontent.com/ffbsoffa/TapeXPlayer/main/source/install.ps1 | iex

================================================================================
SYSTEM REQUIREMENTS / СИСТЕМНЫЕ ТРЕБОВАНИЯ
================================================================================

• Windows 10 (version 1903+) or Windows 11
• x86_64 (64-bit) processor
• 4 GB RAM minimum, 8 GB recommended
• DirectX 11 compatible GPU

================================================================================
INCLUDED LIBRARIES / ВСТРОЕННЫЕ БИБЛИОТЕКИ
================================================================================

All runtime libraries are included in this package:
• FFmpeg (avcodec, avformat, avutil, swscale, swresample) — LGPL 2.1+
• SDL2 — zlib License
• SDL2_ttf — zlib License
• PortAudio — MIT License
• OpenSSL — Apache License 2.0

See LICENSES_THIRD_PARTY.txt for full license texts.

IMPORTANT: FFmpeg is dynamically linked in compliance with LGPL.
You may replace the bundled FFmpeg DLLs with your own compiled version.

================================================================================
SUPPORTED FORMATS / ПОДДЕРЖИВАЕМЫЕ ФОРМАТЫ
================================================================================

Video: MP4, MOV, AVI, MKV, H.264, H.265/HEVC, ProRes, DNxHD, and all
       formats supported by FFmpeg.

Audio: MP3, WAV, FLAC, AAC, M4A, OGG, multi-channel audio.

================================================================================
FEATURES / ВОЗМОЖНОСТИ
================================================================================

• Frame-accurate video playback
• Hardware acceleration (DXVA2/D3D11)
• Variable speed: 0.1x — 32x (forward/reverse)
• Low-resolution proxy caching for smooth scrubbing
• Multiple player instances
• Timecode-based seeking (HH:MM:SS:FF)
• Memory Locations for quick navigation
• MIDI controller support (Mackie HUI / X-Touch One)
• Configurable audio device and buffer size
• VU meters for audio level monitoring

================================================================================
SHORTCUTS / ГОРЯЧИЕ КЛАВИШИ
================================================================================

Space          Play/Pause
P              Play
S              Stop
R              Reverse toggle
Up / Down      Speed step up/down
1              Set speed 1x
2              Set speed 3x
Left / Right   Seek -10s / +10s  (Shift: -1min / +1min)
Home / End     Go to start / end
Ctrl+G         Timecode seek
Ctrl+O         Open file
Ctrl+N         New window
Ctrl+C         Screenshot
Ctrl+,         Settings
Ctrl+I         Inspector
Ctrl+Shift+M   Memory Locations
Ctrl+Q         Quit

================================================================================
TROUBLESHOOTING / РЕШЕНИЕ ПРОБЛЕМ
================================================================================

Q: "Windows protected your PC" (SmartScreen warning)
A: Click "More info" → "Run anyway"
   The application is open-source and safe to run.

Q: Missing DLL error
A: All required DLLs are included in this package. Make sure you're running
   TapeXPlayer.exe from the extracted folder, not a moved copy.

Q: Video doesn't play
A: Check the codec. The application supports all standard formats via FFmpeg.

Q: Poor performance
A: Open Settings (Ctrl+,) → Video & Sync and adjust parameters.
   The application auto-creates low-resolution proxies for smooth scrubbing.

Q: MIDI controller not detected
A: Settings (Ctrl+,) → MIDI tab → Enable MIDI Controller → select ports.
   Supports: Mackie HUI / X-Touch One

================================================================================
LINKS / ССЫЛКИ
================================================================================

• GitHub:    https://github.com/ffbsoffa/TapeXPlayer
• Issues:    https://github.com/ffbsoffa/TapeXPlayer/issues

================================================================================
LICENSE / ЛИЦЕНЗИЯ
================================================================================

TapeXPlayer is open source. See LICENSE in the repository.
Third-party library licenses: see LICENSES_THIRD_PARTY.txt

================================================================================
                        Thank you for using TapeXPlayer!
================================================================================

Build Date: $(date +"%B %d, %Y")
Build Number: ${BUILD}
Architecture: x86_64
EOF

# ── Create ZIP ────────────────────────────────────────────────────────────────
echo "6️⃣   Creating ZIP archive..."
rm -f "$RELEASE_ZIP"

cd "$RELEASE_DIR"
if command -v zip &>/dev/null; then
    zip -r -q "../../$(basename "$RELEASE_ZIP")" "$(basename "$RELEASE_CONTENT")"
else
    # PowerShell fallback (when running on Windows without zip)
    powershell -Command "Compress-Archive -Path '$(basename "$RELEASE_CONTENT")' -DestinationPath '../../$(basename "$RELEASE_ZIP")' -Force"
fi
cd - > /dev/null

# ── Cleanup ───────────────────────────────────────────────────────────────────
rm -rf "$RELEASE_DIR"

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "✅  Windows release created!"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "  File: $RELEASE_ZIP"
if [ -f "$RELEASE_ZIP" ]; then
    SIZE=$(du -h "$RELEASE_ZIP" 2>/dev/null | cut -f1 || echo "?")
    echo "  Size: $SIZE"
fi
echo ""
echo "  Contents:"
echo "    • TapeXPlayer.exe"
echo "    • $DLL_COUNT runtime DLLs (fully self-contained)"
echo "    • install.ps1"
echo "    • uninstall.ps1"
echo "    • README.txt (EN/RU)"
echo "    • LICENSES_THIRD_PARTY.txt"
echo ""
echo "  Ready to upload to GitHub Releases! 🚀"
echo ""
