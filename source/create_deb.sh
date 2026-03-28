#!/bin/bash
# TapeXPlayer DEB Package Creator
# Creates a Debian package for Ubuntu/Debian distributions

set -e  # Exit on error

echo "================================================"
echo "TapeXPlayer DEB Package Creator"
echo "================================================"
echo ""

# Version information - read from VERSION file
VERSION_FILE="../VERSION"
if [ -f "$VERSION_FILE" ]; then
    source "$VERSION_FILE"
    echo "Loaded version: $VERSION (Code name: $CODE_NAME)"
else
    VERSION="2026.01"
    CODE_NAME="Albatross"
    echo "WARNING: VERSION file not found, using defaults"
fi

# Read build number from makefile's .build_number if it exists
BUILD_NUMBER_FILE=".build_number"
if [ -f "$BUILD_NUMBER_FILE" ]; then
    BUILD_NUMBER=$(cat "$BUILD_NUMBER_FILE")
    echo "Found build number: $BUILD_NUMBER"
else
    BUILD_NUMBER=$(date +"%Y%m%d")
    echo "No build number file, using date: $BUILD_NUMBER"
fi

PACKAGE_VERSION="${VERSION}.${BUILD_NUMBER}"

# Auto-detect architecture
ARCH=$(dpkg --print-architecture)
PACKAGE_NAME="tapexplayer_${PACKAGE_VERSION}_${ARCH}"

# Directories
BUILD_DIR="../builds/binaries/deb"
PACKAGE_DIR="${BUILD_DIR}/${PACKAGE_NAME}"
DEBIAN_DIR="${PACKAGE_DIR}/DEBIAN"
BIN_DIR="${PACKAGE_DIR}/usr/bin"
SHARE_DIR="${PACKAGE_DIR}/usr/share"
ICON_DIR="${SHARE_DIR}/icons/hicolor"
DESKTOP_DIR="${SHARE_DIR}/applications"
DOC_DIR="${SHARE_DIR}/doc/tapexplayer"

echo "Package: ${PACKAGE_NAME}"
echo "Version: ${PACKAGE_VERSION}"
echo "Architecture: ${ARCH}"
echo ""

# Clean old build
echo "Cleaning old build..."
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

# Create directory structure
echo "Creating directory structure..."
mkdir -p "${DEBIAN_DIR}"
mkdir -p "${BIN_DIR}"
mkdir -p "${DESKTOP_DIR}"
mkdir -p "${DOC_DIR}"
mkdir -p "${ICON_DIR}/16x16/apps"
mkdir -p "${ICON_DIR}/32x32/apps"
mkdir -p "${ICON_DIR}/48x48/apps"
mkdir -p "${ICON_DIR}/128x128/apps"
mkdir -p "${ICON_DIR}/256x256/apps"
mkdir -p "${ICON_DIR}/512x512/apps"
mkdir -p "${ICON_DIR}/1024x1024/apps"

# Check if executable exists
if [ ! -f "../builds/binaries/TapeXPlayer_linux" ]; then
    echo "❌ Error: Executable not found!"
    echo "   Please run 'make' first to build the application."
    exit 1
fi

# Copy executable
echo "Copying executable..."
cp ../builds/binaries/TapeXPlayer_linux "${BIN_DIR}/tapexplayer"
chmod 755 "${BIN_DIR}/tapexplayer"

# Copy desktop file
echo "Copying desktop file..."
cp ../resources/tapexplayer.desktop "${DESKTOP_DIR}/"
chmod 644 "${DESKTOP_DIR}/tapexplayer.desktop"

# Copy icons
echo "Copying icons..."
cp ../resources/icons/tapexplayer_16.png "${ICON_DIR}/16x16/apps/tapexplayer.png"
cp ../resources/icons/tapexplayer_32.png "${ICON_DIR}/32x32/apps/tapexplayer.png"
cp ../resources/icons/tapexplayer_48.png "${ICON_DIR}/48x48/apps/tapexplayer.png"
cp ../resources/icons/tapexplayer_128.png "${ICON_DIR}/128x128/apps/tapexplayer.png"
cp ../resources/icons/tapexplayer_256.png "${ICON_DIR}/256x256/apps/tapexplayer.png"
cp ../resources/icons/tapexplayer_512.png "${ICON_DIR}/512x512/apps/tapexplayer.png"
cp ../resources/icons/tapexplayer_1024.png "${ICON_DIR}/1024x1024/apps/tapexplayer.png"

# Create copyright file
echo "Creating copyright file..."
cat > "${DOC_DIR}/copyright" << 'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: TapeXPlayer
Upstream-Contact: TapeXPlayer Development Team
Source: https://github.com/yourusername/tapexplayer

Files: *
Copyright: 2026 TapeXPlayer Development Team
License: Custom
 [Add your license text here]
EOF

# Create changelog
echo "Creating changelog..."
cat > "${DOC_DIR}/changelog" << EOF
tapexplayer (${PACKAGE_VERSION}) unstable; urgency=medium

  * Initial release for Linux
  * Frame-accurate video playback
  * GTK+3 integration
  * Hardware-accelerated decoding
  * Multiple format support
  * Memory locations feature
  * Screenshot functionality
  * Heap corruption fixes for Intel Celeron

 -- FFB_soffa <mail@ffbsoffa.org>  $(date -R)
EOF
gzip -9 "${DOC_DIR}/changelog"

# Create README
echo "Creating README..."
cat > "${DOC_DIR}/README" << 'EOF'
TapeXPlayer 2026
================

Professional frame-accurate video player for Linux.

Features:
- Frame-accurate playback and scrubbing
- Hardware-accelerated decoding (VAAPI)
- GTK+3 native dialogs
- Memory locations (bookmarks)
- Screenshot with timecode overlay
- Multiple video and audio formats
- MIDI control support

Usage:
  tapexplayer [file]

Keyboard shortcuts:
- Space:       Play/Pause
- Left/Right:  Frame step
- Up/Down:     Speed control
- Ctrl+O:      Open file
- Ctrl+C:      Take screenshot
- Ctrl+M:      Add memory location
- ESC/Ctrl+Q:  Exit

For more information, visit: https://github.com/ffbsoffa/tapexplayer
EOF

# Calculate installed size
INSTALLED_SIZE=$(du -sk "${PACKAGE_DIR}/usr" | cut -f1)

# Create control file
echo "Creating control file..."

# Prepare description with code name if available
if [ -n "$CODE_NAME" ]; then
    SHORT_DESC="Professional frame-accurate video player (${CODE_NAME})"
else
    SHORT_DESC="Professional frame-accurate video player"
fi

cat > "${DEBIAN_DIR}/control" << EOF
Package: tapexplayer
Version: ${PACKAGE_VERSION}
Section: video
Priority: optional
Architecture: ${ARCH}
Depends: libsdl2-2.0-0 (>= 2.0.0), libsdl2-ttf-2.0-0, libportaudio2, libavformat61 | libavformat60 | libavformat59 | libavformat58, libavcodec61 | libavcodec60 | libavcodec59 | libavcodec58, libavutil59 | libavutil58 | libavutil57 | libavutil56, libswscale8 | libswscale7 | libswscale6 | libswscale5, libssl3 | libssl1.1, librtmidi7 | librtmidi6 | librtmidi4, libgtk-3-0, libglib2.0-0, libpango-1.0-0, libcairo2
Recommends: vaapi-driver-all
Suggests: ffmpeg
Installed-Size: ${INSTALLED_SIZE}
Maintainer: FFB_soffa <mail@ffbsoffa.org>
Homepage: https://github.com/ffbsoffa/TapeXPlayer
Description: ${SHORT_DESC}
 TapeXPlayer ${VERSION} "${CODE_NAME}" is a professional video player
 designed for frame-accurate playback, video editing, and post-production
 workflows.
 .
 Features:
  * Frame-accurate playback and scrubbing
  * Hardware-accelerated decoding (VAAPI support)
  * GTK+3 native integration
  * Memory locations (bookmarks with timecode)
  * Screenshot functionality with timecode overlay
  * Multiple video and audio format support
  * MIDI control support for professional workflows
  * Optimized for Intel Celeron and low-power systems
EOF

# Create postinst script (post-installation)
echo "Creating postinst script..."
cat > "${DEBIAN_DIR}/postinst" << 'EOF'
#!/bin/bash
set -e

# Update desktop database
if command -v update-desktop-database &> /dev/null; then
    update-desktop-database -q /usr/share/applications || true
fi

# Update icon cache
if command -v gtk-update-icon-cache &> /dev/null; then
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi

echo "TapeXPlayer installed successfully!"
echo "Launch from applications menu or run: tapexplayer"

exit 0
EOF
chmod 755 "${DEBIAN_DIR}/postinst"

# Create postrm script (post-removal)
echo "Creating postrm script..."
cat > "${DEBIAN_DIR}/postrm" << 'EOF'
#!/bin/bash
set -e

if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    # Update desktop database
    if command -v update-desktop-database &> /dev/null; then
        update-desktop-database -q /usr/share/applications || true
    fi

    # Update icon cache
    if command -v gtk-update-icon-cache &> /dev/null; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
fi

exit 0
EOF
chmod 755 "${DEBIAN_DIR}/postrm"

# Build the package
echo ""
echo "Building DEB package..."
dpkg-deb --root-owner-group --build "${PACKAGE_DIR}"

# Show package info
echo ""
echo "================================================"
echo "✅ DEB package created successfully!"
echo "================================================"
echo ""
echo "Package: ${BUILD_DIR}/${PACKAGE_NAME}.deb"
echo "Size:    $(du -h ${BUILD_DIR}/${PACKAGE_NAME}.deb | cut -f1)"
echo ""
echo "Package information:"
dpkg-deb --info "${BUILD_DIR}/${PACKAGE_NAME}.deb"
echo ""
echo "Package contents:"
dpkg-deb --contents "${BUILD_DIR}/${PACKAGE_NAME}.deb" | head -20
echo "..."
echo ""
echo "To install:"
echo "  sudo dpkg -i ${BUILD_DIR}/${PACKAGE_NAME}.deb"
echo "  sudo apt-get install -f  # Install dependencies if needed"
echo ""
echo "To remove:"
echo "  sudo apt-get remove tapexplayer"
echo ""
