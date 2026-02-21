#!/bin/bash
# install.sh — TapeXPlayer macOS Installer
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/ffbsoffa/TapeXPlayer/main/source/install.sh | bash
#   curl -fsSL ... | bash -s -- --quiet
#   curl -fsSL ... | bash -s -- --force
#
# Flags:
#   --quiet   Non-interactive mode (no prompts, auto-overwrite)
#   --force   Force reinstall even if same version is detected

set -e

# ── Configuration ─────────────────────────────────────────────────────────────
GITHUB_REPO="ffbsoffa/TapeXPlayer"
APP_NAME="TapeXPlayer"
MIN_MACOS_MAJOR=13   # macOS 13.0 Ventura
INSTALL_DIR="/Applications"

# ── Flags ─────────────────────────────────────────────────────────────────────
QUIET=0
FORCE=0
for arg in "$@"; do
    case "$arg" in
        --quiet) QUIET=1 ;;
        --force) FORCE=1 ;;
    esac
done

# ── Helpers ───────────────────────────────────────────────────────────────────
print_header() {
    echo ""
    echo "════════════════════════════════════════════════════════════════"
    echo "           TapeXPlayer Installer for macOS"
    echo "════════════════════════════════════════════════════════════════"
    echo ""
}

info()    { echo "   $*"; }
success() { echo "   ✅ $*"; }
warn()    { echo "   ⚠️  $*"; }
fail()    {
    echo ""
    echo "   ❌ $*"
    echo ""
    exit 1
}

ask() {
    # ask <prompt> → returns 0 for yes, 1 for no
    # In quiet mode always returns 0 (yes)
    if [ "$QUIET" -eq 1 ]; then return 0; fi
    local reply
    read -r -p "   $1 (y/n): " reply
    [[ "$reply" =~ ^[Yy]$ ]]
}

# ── macOS version check ───────────────────────────────────────────────────────
check_macos_version() {
    local version
    version=$(sw_vers -productVersion 2>/dev/null || echo "0.0")
    local major
    major=$(echo "$version" | cut -d. -f1)

    if [ "$major" -lt "$MIN_MACOS_MAJOR" ] 2>/dev/null; then
        fail "macOS $MIN_MACOS_MAJOR.0 (Ventura) or later required. Found: $version"
    fi

    local arch
    arch=$(uname -m)
    info "macOS $version detected ($arch)"
}

# ── Dependency check ──────────────────────────────────────────────────────────
check_deps() {
    for cmd in curl unzip; do
        if ! command -v "$cmd" &>/dev/null; then
            fail "Required tool not found: $cmd"
        fi
    done
}

# ── Fetch latest release URL ──────────────────────────────────────────────────
get_download_url() {
    info "Fetching latest release from GitHub..."

    local api_url="https://api.github.com/repos/${GITHUB_REPO}/releases/latest"
    local response
    response=$(curl -fsSL "$api_url" 2>/dev/null) || \
        fail "Cannot reach GitHub API. Check your internet connection."

    # Extract browser_download_url for macOS ZIP (matches *-mac.zip or TapeXPlayer-*.zip without -win/-linux)
    local url
    url=$(printf '%s' "$response" \
        | grep '"browser_download_url"' \
        | grep -i '\-mac\.zip\|TapeXPlayer.*\.zip' \
        | grep -iv '\-win\|\-linux' \
        | head -1 \
        | sed 's/.*"browser_download_url": *"\([^"]*\)".*/\1/')

    if [ -z "$url" ]; then
        # Fallback: any .zip in the release
        url=$(printf '%s' "$response" \
            | grep '"browser_download_url"' \
            | grep '\.zip' \
            | grep -iv '\-win\|\-linux' \
            | head -1 \
            | sed 's/.*"browser_download_url": *"\([^"]*\)".*/\1/')
    fi

    if [ -z "$url" ]; then
        fail "No macOS ZIP found in the latest GitHub release.\nCheck: https://github.com/${GITHUB_REPO}/releases"
    fi

    echo "$url"
}

# ── Optional SHA256 verification ─────────────────────────────────────────────
verify_sha256() {
    local file="$1"
    local sha_url="$2"

    local expected
    expected=$(curl -fsSL "$sha_url" 2>/dev/null | awk '{print $1}') || return 0

    if [ -z "$expected" ]; then return 0; fi   # no checksum file — skip

    local actual
    actual=$(shasum -a 256 "$file" | awk '{print $1}')

    if [ "$actual" != "$expected" ]; then
        fail "SHA256 mismatch!\n   Expected: $expected\n   Actual:   $actual\n   The download may be corrupted."
    fi

    success "SHA256 verified"
}

# ── Validate bundle is self-contained ────────────────────────────────────────
validate_bundle() {
    local app="$1"
    local exe="$app/Contents/MacOS/${APP_NAME}"

    # 1. Check Frameworks dir has embedded libavcodec
    if ! ls "$app/Contents/Frameworks/libavcodec"*.dylib 1>/dev/null 2>&1; then
        fail "Broken archive: embedded libavcodec not found in Frameworks/\n   The download may be incomplete or corrupted."
    fi

    # 2. Check that ALL FFmpeg/SDL2 libs in the binary point to @executable_path,
    #    NOT to /opt/homebrew, /usr/local, or any absolute external path.
    #    This is the critical check: even one unpatched reference means the app
    #    will try to load the user's system library instead of the bundled one.
    local bad_refs
    bad_refs=$(otool -L "$exe" 2>/dev/null \
        | grep -E "libavcodec|libavformat|libavutil|libswscale|libswresample|libSDL2|libportaudio|librtmidi|libssl|libcrypto" \
        | grep -v "@executable_path" \
        | awk '{print $1}')

    if [ -n "$bad_refs" ]; then
        fail "Binary is NOT self-contained — these libs still reference external paths:\n\n${bad_refs}\n\n   This means the app will fail to launch on machines without Homebrew\n   or with a different FFmpeg version. The release was built incorrectly\n   (bundle_libs.sh was not run, or install_name_tool patching failed).\n\n   Please report this at: https://github.com/${GITHUB_REPO}/issues"
    fi

    # 3. Verify the bundled Frameworks libs themselves don't reference external paths
    #    (transitive deps must also be patched)
    local fw_dir="$app/Contents/Frameworks"
    local fw_bad=""
    for dylib in "$fw_dir"/lib{avcodec,avformat,avutil,swscale,swresample}*.dylib; do
        [ -f "$dylib" ] || continue
        local ext_refs
        ext_refs=$(otool -L "$dylib" 2>/dev/null \
            | tail -n +2 \
            | grep -v "@executable_path\|/System/\|/usr/lib/" \
            | grep -v "$(basename "$dylib")" \
            | awk '{print $1}')
        if [ -n "$ext_refs" ]; then
            fw_bad="$fw_bad\n  $(basename "$dylib"): $ext_refs"
        fi
    done

    if [ -n "$fw_bad" ]; then
        warn "Some bundled FFmpeg libs have unpatched transitive deps:${fw_bad}"
        warn "The app may fail if those deps are missing on the target system."
        warn "Consider rebuilding with 'make bundle-universal' and re-releasing."
        # Not fatal — warn only, because the main exe paths are correct
    fi

    success "Bundle is self-contained (all primary libs embedded and patched)"
}

# ── Main install logic ────────────────────────────────────────────────────────
print_header
check_macos_version
check_deps

DOWNLOAD_URL=$(get_download_url)
FILENAME=$(basename "$DOWNLOAD_URL")
info "Latest release: $FILENAME"

# Create temp directory
TMPDIR_INSTALL=$(mktemp -d)
trap 'rm -rf "$TMPDIR_INSTALL"' EXIT

ZIP_PATH="$TMPDIR_INSTALL/$FILENAME"

echo ""
echo "1️⃣   Downloading..."
info "URL: $DOWNLOAD_URL"
curl -fL --progress-bar "$DOWNLOAD_URL" -o "$ZIP_PATH" || \
    fail "Download failed."

# Optional SHA256 check
SHA_URL="${DOWNLOAD_URL%.zip}.sha256"
verify_sha256 "$ZIP_PATH" "$SHA_URL"

echo ""
echo "2️⃣   Extracting..."
unzip -q "$ZIP_PATH" -d "$TMPDIR_INSTALL/"

# Find the .app bundle
APP_FOUND=$(find "$TMPDIR_INSTALL" -maxdepth 3 -name "${APP_NAME}.app" -type d | head -1)
if [ -z "$APP_FOUND" ]; then
    fail "${APP_NAME}.app not found in the downloaded archive."
fi
success "Found: $(basename "$APP_FOUND")"

echo ""
echo "3️⃣   Removing Gatekeeper quarantine..."
xattr -cr "$APP_FOUND" 2>/dev/null || true
success "Quarantine removed"

echo ""
echo "4️⃣   Validating bundle integrity..."
validate_bundle "$APP_FOUND"

echo ""
echo "5️⃣   Installing to $INSTALL_DIR..."
DEST="$INSTALL_DIR/${APP_NAME}.app"

if [ -d "$DEST" ]; then
    if [ "$FORCE" -eq 0 ] && [ "$QUIET" -eq 0 ]; then
        warn "${APP_NAME} is already installed."
        if ! ask "Replace existing version?"; then
            echo ""
            info "Installation cancelled."
            info "Existing version untouched at: $DEST"
            exit 0
        fi
    fi
    info "Removing old version..."
    rm -rf "$DEST"
fi

# Try copying without sudo first; fall back to sudo if needed
if cp -R "$APP_FOUND" "$DEST" 2>/dev/null; then
    success "Installed to $DEST"
else
    info "Requesting administrator access to write to $INSTALL_DIR..."
    sudo cp -R "$APP_FOUND" "$DEST" && success "Installed to $DEST (with sudo)" || \
        fail "Installation failed. Try copying manually:\n   cp -R \"$APP_FOUND\" \"$DEST\""
fi

echo ""
echo "════════════════════════════════════════════════════════════════"
success "Installation complete!"
echo "════════════════════════════════════════════════════════════════"
echo ""
info "TapeXPlayer installed to: $DEST"
echo ""

if [ "$QUIET" -eq 0 ]; then
    if ask "Launch TapeXPlayer now?"; then
        open -a "${APP_NAME}"
    fi
fi

echo ""
