#!/bin/bash

# TapeXPlayer 2026 - Installation Script
# Supports: macOS (arm64/x86_64), Linux (x86_64)

set -e

INSTALL_VERSION="2026.01"
DOWNLOAD_BASE_URL="https://example.com/releases"  # TODO: Replace with actual URL

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║         TapeXPlayer 2026 - Installation Script          ║"
echo "║                    Version ${INSTALL_VERSION}                      ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

# Detect platform
detect_platform() {
    OS=$(uname -s)
    ARCH=$(uname -m)

    case "$OS" in
        Darwin)
            PLATFORM="macos"
            case "$ARCH" in
                arm64)
                    BINARY_NAME="TapeXPlayer_macos_arm64.app.tar.gz"
                    ;;
                x86_64)
                    BINARY_NAME="TapeXPlayer_macos_x86_64.app.tar.gz"
                    ;;
                *)
                    echo -e "${RED}Error: Unsupported macOS architecture: $ARCH${NC}"
                    exit 1
                    ;;
            esac
            ;;
        Linux)
            PLATFORM="linux"
            case "$ARCH" in
                x86_64)
                    BINARY_NAME="TapeXPlayer_linux_x86_64.tar.gz"
                    ;;
                *)
                    echo -e "${RED}Error: Unsupported Linux architecture: $ARCH${NC}"
                    exit 1
                    ;;
            esac
            ;;
        *)
            echo -e "${RED}Error: Unsupported operating system: $OS${NC}"
            exit 1
            ;;
    esac

    echo -e "${GREEN}✓${NC} Detected platform: ${BLUE}$PLATFORM ($ARCH)${NC}"
}

# Check for download tool (curl or wget)
check_download_tool() {
    if command -v curl &> /dev/null; then
        DOWNLOADER="curl"
        DOWNLOAD_CMD="curl -fL -o"
        echo -e "${GREEN}✓${NC} Found download tool: ${BLUE}curl${NC}"
    elif command -v wget &> /dev/null; then
        DOWNLOADER="wget"
        DOWNLOAD_CMD="wget -O"
        echo -e "${GREEN}✓${NC} Found download tool: ${BLUE}wget${NC}"
    else
        echo -e "${RED}✗ Error: Neither curl nor wget found${NC}"
        echo "  Please install curl or wget first:"
        echo "    macOS:  brew install curl"
        echo "    Linux:  sudo apt install curl  (or yum/dnf install curl)"
        exit 1
    fi
}

# Download and install for macOS
install_macos() {
    echo ""
    echo -e "${YELLOW}Installing TapeXPlayer for macOS...${NC}"
    echo ""

    # Create temporary directory
    TMP_DIR=$(mktemp -d)
    trap "rm -rf $TMP_DIR" EXIT

    cd "$TMP_DIR"

    # Download
    DOWNLOAD_URL="${DOWNLOAD_BASE_URL}/${BINARY_NAME}"
    echo -e "${BLUE}→${NC} Downloading from: $DOWNLOAD_URL"

    if ! $DOWNLOAD_CMD "$BINARY_NAME" "$DOWNLOAD_URL"; then
        echo -e "${RED}✗ Download failed${NC}"
        echo ""
        echo "Manual installation:"
        echo "  1. Download from: $DOWNLOAD_URL"
        echo "  2. Extract: tar -xzf $BINARY_NAME"
        echo "  3. Move to Applications: mv TapeXPlayer.app /Applications/"
        echo "  4. Remove quarantine: xattr -dr com.apple.quarantine /Applications/TapeXPlayer.app"
        exit 1
    fi

    echo -e "${GREEN}✓${NC} Download complete"

    # Extract
    echo -e "${BLUE}→${NC} Extracting archive..."
    tar -xzf "$BINARY_NAME"

    # Determine installation directory
    if [ -w "/Applications" ]; then
        INSTALL_DIR="/Applications"
    else
        INSTALL_DIR="$HOME/Applications"
        mkdir -p "$INSTALL_DIR"
    fi

    # Remove old version if exists
    if [ -d "$INSTALL_DIR/TapeXPlayer.app" ]; then
        echo -e "${YELLOW}→${NC} Removing old version..."
        rm -rf "$INSTALL_DIR/TapeXPlayer.app"
    fi

    # Install
    echo -e "${BLUE}→${NC} Installing to: $INSTALL_DIR"
    mv TapeXPlayer.app "$INSTALL_DIR/"

    # CRITICAL: Remove quarantine attribute to bypass GateKeeper
    # This is the key difference from browser downloads
    echo -e "${BLUE}→${NC} Removing quarantine attribute (bypass GateKeeper)..."
    xattr -dr com.apple.quarantine "$INSTALL_DIR/TapeXPlayer.app" 2>/dev/null || true

    # Make executable
    chmod +x "$INSTALL_DIR/TapeXPlayer.app/Contents/MacOS/TapeXPlayer"

    echo ""
    echo -e "${GREEN}╔══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║           ✓ Installation Complete (macOS)               ║${NC}"
    echo -e "${GREEN}╚══════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "${BLUE}Location:${NC} $INSTALL_DIR/TapeXPlayer.app"
    echo ""
    echo -e "${GREEN}You can now launch TapeXPlayer from:${NC}"
    echo "  • Finder → Applications → TapeXPlayer"
    echo "  • Spotlight (⌘+Space) → TapeXPlayer"
    echo ""
    echo -e "${YELLOW}Note:${NC} GateKeeper bypass enabled - app will run without warnings"
}

# Download and install for Linux
install_linux() {
    echo ""
    echo -e "${YELLOW}Installing TapeXPlayer for Linux...${NC}"
    echo ""

    # Create temporary directory
    TMP_DIR=$(mktemp -d)
    trap "rm -rf $TMP_DIR" EXIT

    cd "$TMP_DIR"

    # Download
    DOWNLOAD_URL="${DOWNLOAD_BASE_URL}/${BINARY_NAME}"
    echo -e "${BLUE}→${NC} Downloading from: $DOWNLOAD_URL"

    if ! $DOWNLOAD_CMD "$BINARY_NAME" "$DOWNLOAD_URL"; then
        echo -e "${RED}✗ Download failed${NC}"
        echo ""
        echo "Manual installation:"
        echo "  1. Download from: $DOWNLOAD_URL"
        echo "  2. Extract: tar -xzf $BINARY_NAME"
        echo "  3. Move binary: sudo mv TapeXPlayer_linux /usr/local/bin/tapexplayer"
        exit 1
    fi

    echo -e "${GREEN}✓${NC} Download complete"

    # Extract
    echo -e "${BLUE}→${NC} Extracting archive..."
    tar -xzf "$BINARY_NAME"

    # Determine installation directory
    if [ -w "/usr/local/bin" ]; then
        INSTALL_DIR="/usr/local/bin"
        DESKTOP_DIR="/usr/share/applications"
        NEED_SUDO=false
    else
        INSTALL_DIR="$HOME/.local/bin"
        DESKTOP_DIR="$HOME/.local/share/applications"
        NEED_SUDO=false
        mkdir -p "$INSTALL_DIR"
        mkdir -p "$DESKTOP_DIR"
    fi

    # Install binary
    echo -e "${BLUE}→${NC} Installing to: $INSTALL_DIR"

    if [ "$NEED_SUDO" = true ] && [ -w "/usr/local/bin" ]; then
        sudo mv TapeXPlayer_linux "$INSTALL_DIR/tapexplayer"
        sudo chmod +x "$INSTALL_DIR/tapexplayer"
    elif [ ! -w "/usr/local/bin" ] && command -v sudo &> /dev/null; then
        echo -e "${YELLOW}→${NC} Need sudo permission for system-wide installation..."
        sudo mv TapeXPlayer_linux "$INSTALL_DIR/tapexplayer"
        sudo chmod +x "$INSTALL_DIR/tapexplayer"
    else
        mv TapeXPlayer_linux "$INSTALL_DIR/tapexplayer"
        chmod +x "$INSTALL_DIR/tapexplayer"
    fi

    # Create desktop entry
    echo -e "${BLUE}→${NC} Creating desktop entry..."

    DESKTOP_FILE="$DESKTOP_DIR/tapexplayer.desktop"

    cat > tapexplayer.desktop << 'EOF'
[Desktop Entry]
Version=1.0
Type=Application
Name=TapeXPlayer 2026
GenericName=Video Player
Comment=Professional video player with frame-accurate control
Exec=tapexplayer %F
Icon=tapexplayer
Terminal=false
Categories=AudioVideo;Player;
MimeType=video/mp4;video/x-matroska;video/quicktime;video/x-msvideo;
Keywords=video;player;playback;
EOF

    if [ "$NEED_SUDO" = true ] || [ ! -w "$DESKTOP_DIR" ]; then
        sudo mv tapexplayer.desktop "$DESKTOP_FILE"
        sudo chmod 644 "$DESKTOP_FILE"
    else
        mv tapexplayer.desktop "$DESKTOP_FILE"
        chmod 644 "$DESKTOP_FILE"
    fi

    # Update desktop database
    if command -v update-desktop-database &> /dev/null; then
        if [ -w "$DESKTOP_DIR" ]; then
            update-desktop-database "$DESKTOP_DIR" 2>/dev/null || true
        else
            sudo update-desktop-database "$DESKTOP_DIR" 2>/dev/null || true
        fi
    fi

    echo ""
    echo -e "${GREEN}╔══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║           ✓ Installation Complete (Linux)               ║${NC}"
    echo -e "${GREEN}╚══════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "${BLUE}Location:${NC} $INSTALL_DIR/tapexplayer"
    echo ""
    echo -e "${GREEN}You can now launch TapeXPlayer from:${NC}"
    echo "  • Command line: tapexplayer"
    echo "  • Application menu (GNOME/KDE/etc.)"
    echo ""

    # Add to PATH reminder
    if [ "$INSTALL_DIR" = "$HOME/.local/bin" ]; then
        if [[ ":$PATH:" != *":$HOME/.local/bin:"* ]]; then
            echo -e "${YELLOW}Note:${NC} Add ~/.local/bin to your PATH:"
            echo "  echo 'export PATH=\"\$HOME/.local/bin:\$PATH\"' >> ~/.bashrc"
            echo "  source ~/.bashrc"
            echo ""
        fi
    fi
}

# Main installation flow
main() {
    detect_platform
    check_download_tool

    case "$PLATFORM" in
        macos)
            install_macos
            ;;
        linux)
            install_linux
            ;;
    esac
}

# Run installation
main
