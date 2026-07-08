#!/bin/bash
# create_release.sh - Create release archive for GitHub
# Usage: ./create_release.sh <build> <codename>

set -e

BUILD="$1"
CODENAME="$2"

if [ -z "$BUILD" ]; then
    echo "Usage: $0 <build> <codename>"
    echo "Example: $0 1020 Albatross"
    exit 1
fi

APP_PATH="../builds/TapeXPlayer.app"
if [ ! -d "$APP_PATH" ]; then
    echo "❌ Error: $APP_PATH not found"
    echo "First run: make bundle"
    exit 1
fi

# Determine release name
if [ -n "$CODENAME" ]; then
    RELEASE_NAME="TapeXPlayer-${CODENAME}-build${BUILD}-mac"
else
    RELEASE_NAME="TapeXPlayer-build${BUILD}-mac"
fi

RELEASE_DIR="../builds/release"
RELEASE_CONTENT="$RELEASE_DIR/$RELEASE_NAME"
RELEASE_ZIP="../builds/${RELEASE_NAME}.zip"

echo "📦 Creating GitHub Release: $RELEASE_NAME"
echo ""

# Clean and create folder
rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_CONTENT"

# Copy application
echo "1️⃣  Copying TapeXPlayer.app..."
cp -R "$APP_PATH" "$RELEASE_CONTENT/"

# Copy LICENSES file (LGPL compliance)
if [ -f "LICENSES_THIRD_PARTY.txt" ]; then
    cp "LICENSES_THIRD_PARTY.txt" "$RELEASE_CONTENT/"
fi

# Create README
echo "2️⃣  Generating README.txt..."
cat > "$RELEASE_CONTENT/README.txt" << EOF
================================================================================
                    TapeXPlayer "${CODENAME}"
                            Build ${BUILD}
================================================================================

Professional Video Player for macOS
macOS 13.0 Ventura or later required
Universal Binary (Intel + Apple Silicon)

================================================================================
INSTALLATION
================================================================================

macOS flags files downloaded from GitHub with a quarantine attribute.
Clear it before the first launch using one of the methods below.

================================================================================
METHOD 1 — Terminal
================================================================================

1. Unpack the ZIP archive
2. Open Terminal (⌘+Space → "Terminal")
3. Run:

   cd ~/Downloads/TapeXPlayer-${CODENAME}-build${BUILD}
   xattr -cr .
   cp -R TapeXPlayer.app /Applications/

4. Launch from Launchpad or /Applications

================================================================================
METHOD 2 — System Settings
================================================================================

1. Unpack the ZIP and move TapeXPlayer.app to /Applications
2. Double-click TapeXPlayer.app
3. macOS will show: "cannot be opened because the developer cannot be verified"
4. Open: System Settings → Privacy & Security
5. Click "Open Anyway" next to TapeXPlayer
6. Confirm launch in the dialog that appears

================================================================================
SYSTEM REQUIREMENTS
================================================================================

• macOS 13.0 Ventura or later
• Apple Silicon (M1/M2/M3) or Intel processor
• 4 GB RAM minimum, 8 GB recommended
• Metal-capable GPU

================================================================================
SUPPORTED FORMATS
================================================================================

Video:
  • MP4, MOV, AVI, MKV
  • H.264, H.265/HEVC, ProRes, DNxHD
  • All formats supported by FFmpeg

Audio:
  • MP3, WAV, FLAC, AAC, M4A, OGG
  • Multi-channel audio support

================================================================================
FEATURES
================================================================================

Playback:
  • Professional frame-accurate video playback
  • Hardware acceleration (VideoToolbox, Metal, FFmpeg)
  • Variable speed: 0.1x - 32x (forward/reverse)
  • Low-resolution proxy caching for smooth scrubbing
  • Multiple player instances support

Control:
  • Timecode-based seeking (HH:MM:SS:FF)
  • Mouse shuttle control (Shift+Ctrl+Drag)
  • Real-time zoom and pan with mouse
  • Memory Locations for quick navigation
  • MIDI controller support (Mackie HUI / X-Touch One)

Audio:
  • Configurable audio device and buffer size
  • Master volume control
  • VU meters for audio level monitoring
  • Multi-channel audio support

Settings (Cmd+,):
  • Audio: Device selection, buffer size, master volume
  • Video & Sync: Frame offset (-10 to +10 frames)
  • MIDI: Enable/disable, input/output port selection
  • Auto-freeze inactive players (resource management)

Additional:
  • Screenshot capture with timecode overlay (Cmd+C)
  • Inspector window for file information
  • Native macOS menu integration
  • SwiftUI-based About and Settings dialogs

================================================================================
SHORTCUTS
================================================================================

Playback:
  Space          Play/Pause (smart logic)
  P              Play
  S              Stop
  R              Reverse toggle

Speed:
  ↑ / ↓          Speed step up/down (1x→3x→10x→18x→24x→32x)
  1              Set speed to 1x
  2              Set speed to 3x
  + / -          Fine speed adjustment

Navigation:
  ← / →          Seek -10s / +10s (Shift: -1min / +1min)
  Home / End     Go to start / end
  Cmd+G          Timecode seek mode
  NumPad *       Timecode seek mode
  Return         Create Memory Location

Zoom:
  Cmd+Z          Zoom In
  Shift+Z        Zoom Out
  X              Reset Zoom
  V              Toggle thumbnail
  Mouse Wheel    Zoom in/out
  Shift+Alt+Drag Pan zoom area

File & Windows:
  Cmd+O          Open file
  Shift+Cmd+O    Open in new instance
  Cmd+N          New window
  Shift+Cmd+N    New window (from Window menu)
  Cmd+W          Close window
  Cmd+M          Minimize window

Tools:
  Cmd+C          Take screenshot
  Cmd+,          Settings (Preferences)
  Cmd+I          Show inspector
  Shift+Cmd+M    Memory Locations
  T              Toggle timecode/frame display
  I              Show info (console)
  Cmd+Q          Quit

Full shortcuts reference: see FSTPKeyboard.cpp in the repository

================================================================================
TROUBLESHOOTING
================================================================================

Q: "TapeXPlayer cannot be opened because the developer cannot be verified"
A: Run: cd ~/Downloads/TapeXPlayer-${CODENAME}-build${BUILD} && xattr -cr .

Q: The app won't launch after installation
A: Make sure you ran xattr -cr before the first launch

Q: Video won't play
A: Check codec support. The app supports all standard formats via FFmpeg.

Q: Poor performance
A: Open Settings → Video & Sync and adjust the parameters. The app
   automatically builds a low-resolution proxy for smooth scrubbing.

Q: How do I connect a MIDI controller?
A: 1. Open Settings (Cmd+,) → MIDI tab
   2. Check "Enable MIDI Controller"
   3. Select the Input/Output ports for your device
   4. Supported: Mackie HUI / X-Touch One
   5. Set your controller to HUI/Mackie mode

Q: How do I sync video with an external monitor?
A: Settings → Video & Sync → Frame Offset
   Use -10 to +10 frames to compensate for monitor latency

Q: I changed Buffer Size in settings but nothing happened
A: Audio buffer size changes require an application restart

================================================================================
LINKS
================================================================================

• GitHub: https://github.com/ffbsoffa/tapexplayer
• Issues: https://github.com/ffbsoffa/tapexplayer/issues
• Documentation: See BUILD_INSTRUCTIONS.md in source
• Keyboard shortcuts: source/modules/FSTPMainModule/WSGUI/FSTPKeyboard.cpp

================================================================================
LICENSE
================================================================================

Open Source - see the LICENSE file in the repository

================================================================================
                        Thank you for using TapeXPlayer!
================================================================================

Build Date: $(date +"%B %d, %Y")
Build Number: ${BUILD}
Architecture: Universal (x86_64 + arm64)

EOF

# Create uninstall script
echo "3️⃣  Creating uninstall.command..."
cat > "$RELEASE_CONTENT/uninstall.command" << 'EOF'
#!/bin/bash
# TapeXPlayer Uninstaller

clear
echo "════════════════════════════════════════════════════════════════"
echo "           TapeXPlayer Uninstaller"
echo "════════════════════════════════════════════════════════════════"
echo ""

if [ ! -d "/Applications/TapeXPlayer.app" ]; then
    echo "TapeXPlayer is not installed in /Applications"
    echo ""
    read -p "Press Enter to exit..."
    exit 0
fi

echo "⚠️  This will remove TapeXPlayer from /Applications"
echo ""
read -p "Continue? (y/n): " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Uninstallation cancelled"
    read -p "Press Enter to exit..."
    exit 0
fi

echo ""
echo "Removing application..."
rm -rf "/Applications/TapeXPlayer.app"
echo "✅ TapeXPlayer removed"
echo ""

echo "Remove settings and cache? (~/Library/Preferences and ~/.fstp)"
read -p "(y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    rm -f ~/Library/Preferences/com.tapexplayer.settings
    rm -rf ~/.fstp
    echo "✅ Settings and cache removed"
fi

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "           ✅ Uninstallation complete"
echo "════════════════════════════════════════════════════════════════"
echo ""
read -p "Press Enter to exit..."
EOF

chmod +x "$RELEASE_CONTENT/uninstall.command"

# Create ZIP
echo "4️⃣  Creating ZIP archive..."
rm -f "$RELEASE_ZIP"
cd "$RELEASE_DIR"
zip -r -q "../../$(basename "$RELEASE_ZIP")" "$(basename "$RELEASE_CONTENT")"
cd - > /dev/null

# Cleanup
rm -rf "$RELEASE_DIR"

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "✅ Release created successfully!"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "File: $RELEASE_ZIP"
echo "Size: $(du -h "$RELEASE_ZIP" | cut -f1)"
echo ""
echo "Contents:"
echo "  • TapeXPlayer.app (Universal Binary)"
echo "  • README.txt (installation instructions)"
echo "  • uninstall.command (removal)"
echo "  • LICENSES_THIRD_PARTY.txt (third-party licenses)"
echo ""
echo "Ready to upload to GitHub Releases! 🚀"
echo ""
