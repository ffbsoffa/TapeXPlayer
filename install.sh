#!/bin/bash
# install.sh -- TapeXPlayer macOS Installer
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/ffbsoffa/TapeXPlayer/stable/install.sh | bash
#   curl -fsSL ... | bash -s -- --quiet
#   curl -fsSL ... | bash -s -- --force
#
# Flags:
#   --quiet   Non-interactive mode (no prompts, auto-overwrite)
#   --force   Force reinstall even if already installed

set -e

# ── Configuration ─────────────────────────────────────────────────────────────
GITHUB_REPO="ffbsoffa/TapeXPlayer"
APP_NAME="TapeXPlayer"
MIN_MACOS_MAJOR=13
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
success() { echo "   OK  $*"; }
warn()    { echo "   !!  $*"; }
fail()    {
    echo ""
    echo "   ERROR: $*"
    echo ""
    exit 1
}

ask() {
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
    info "macOS $version ($arch)"
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
# Note: URL is fetched inline (not via a function) to avoid stdout capture
# swallowing info/fail messages when using $(...) substitution.
fetch_release_url() {
    info "Fetching latest release from GitHub..."

    local api_url="https://api.github.com/repos/${GITHUB_REPO}/releases/latest"
    _API_RESPONSE=$(curl -fsSL "$api_url" 2>/dev/null) || \
        fail "Cannot reach GitHub API. Check your internet connection."

    DOWNLOAD_URL=$(printf '%s' "$_API_RESPONSE" \
        | grep '"browser_download_url"' \
        | grep -i '\-mac\.zip' \
        | head -1 \
        | sed 's/.*"browser_download_url": *"\([^"]*\)".*/\1/' \
        | tr -d '\r\n') || true

    if [ -z "$DOWNLOAD_URL" ]; then
        # Fallback: any .zip not tagged -win or -linux
        DOWNLOAD_URL=$(printf '%s' "$_API_RESPONSE" \
            | grep '"browser_download_url"' \
            | grep '\.zip' \
            | grep -iv '\-win\|\-linux' \
            | head -1 \
            | sed 's/.*"browser_download_url": *"\([^"]*\)".*/\1/' \
            | tr -d '\r\n') || true
    fi

    [ -z "$DOWNLOAD_URL" ] && \
        fail "No macOS ZIP found in the latest release. Check: https://github.com/${GITHUB_REPO}/releases"
}

# ── Optional SHA256 verification ─────────────────────────────────────────────
verify_sha256() {
    local file="$1"
    local sha_url="$2"

    local expected
    expected=$(curl -fsSL "$sha_url" 2>/dev/null | awk '{print $1}' | tr -d '\r\n') || return 0

    if [ -z "$expected" ]; then return 0; fi

    local actual
    actual=$(shasum -a 256 "$file" | awk '{print $1}')

    if [ "$actual" != "$expected" ]; then
        fail "SHA256 mismatch. Expected: $expected  Got: $actual"
    fi

    success "SHA256 verified"
}

# ── Validate bundle is self-contained ────────────────────────────────────────
validate_bundle() {
    local app="$1"
    local exe="$app/Contents/MacOS/${APP_NAME}"

    # Check Frameworks dir has embedded libavcodec
    if ! ls "$app/Contents/Frameworks/libavcodec"*.dylib 1>/dev/null 2>&1; then
        fail "Broken archive: libavcodec not found in Frameworks/. The download may be corrupted."
    fi

    # Check that FFmpeg/SDL2 libs in the binary point to @executable_path, not external paths
    local bad_refs
    bad_refs=$(otool -L "$exe" 2>/dev/null \
        | grep -E "libavcodec|libavformat|libavutil|libswscale|libswresample|libSDL2|libportaudio|librtmidi|libssl|libcrypto" \
        | grep -v "@executable_path" \
        | awk '{print $1}')

    if [ -n "$bad_refs" ]; then
        fail "Binary is not self-contained. These libs reference external paths:
${bad_refs}

   The app will fail to launch without Homebrew or with a different FFmpeg version.
   The release was built incorrectly (bundle_libs.sh was not run).
   Please report this at: https://github.com/${GITHUB_REPO}/issues"
    fi

    # Check transitive deps inside bundled FFmpeg libs (warn only)
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
            fw_bad="$fw_bad  $(basename "$dylib")"
        fi
    done

    if [ -n "$fw_bad" ]; then
        warn "Some bundled FFmpeg libs have unpatched transitive deps:$fw_bad"
        warn "Consider rebuilding with 'make bundle-universal'."
    fi

    success "Bundle is self-contained"
}

# ── Main ──────────────────────────────────────────────────────────────────────
print_header
check_macos_version
check_deps

fetch_release_url   # sets $DOWNLOAD_URL directly, no $() capture
FILENAME=$(basename "$DOWNLOAD_URL")
info "Latest release: $FILENAME"

TMPDIR_INSTALL=$(mktemp -d)
trap 'rm -rf "$TMPDIR_INSTALL"' EXIT

ZIP_PATH="$TMPDIR_INSTALL/$FILENAME"

echo ""
echo "[ 1/5 ] Downloading..."
info "URL: $DOWNLOAD_URL"
curl -fL --progress-bar "$DOWNLOAD_URL" -o "$ZIP_PATH" || \
    fail "Download failed."

SHA_URL="${DOWNLOAD_URL%.zip}.sha256"
verify_sha256 "$ZIP_PATH" "$SHA_URL"

echo ""
echo "[ 2/5 ] Extracting..."
unzip -q "$ZIP_PATH" -d "$TMPDIR_INSTALL/"

APP_FOUND=$(find "$TMPDIR_INSTALL" -maxdepth 3 -name "${APP_NAME}.app" -type d | head -1)
if [ -z "$APP_FOUND" ]; then
    fail "${APP_NAME}.app not found in the downloaded archive."
fi
success "Found: $(basename "$APP_FOUND")"

echo ""
echo "[ 3/5 ] Removing Gatekeeper quarantine..."
xattr -cr "$APP_FOUND" 2>/dev/null || true
success "Quarantine removed"

echo ""
echo "[ 4/5 ] Validating bundle..."
validate_bundle "$APP_FOUND"

echo ""
echo "[ 5/5 ] Installing to $INSTALL_DIR..."
DEST="$INSTALL_DIR/${APP_NAME}.app"

if [ -d "$DEST" ]; then
    if [ "$FORCE" -eq 0 ] && [ "$QUIET" -eq 0 ]; then
        warn "${APP_NAME} is already installed."
        if ! ask "Replace existing version?"; then
            echo ""
            info "Installation cancelled. Existing version untouched at: $DEST"
            exit 0
        fi
    fi
    info "Removing old version..."
    rm -rf "$DEST"
fi

if cp -R "$APP_FOUND" "$DEST" 2>/dev/null; then
    success "Installed to $DEST"
else
    info "Requesting administrator access..."
    sudo cp -R "$APP_FOUND" "$DEST" && success "Installed to $DEST" || \
        fail "Installation failed. Try manually: cp -R \"$APP_FOUND\" \"$DEST\""
fi

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "   Installation complete."
echo "   $DEST"
echo "════════════════════════════════════════════════════════════════"
echo ""

if [ "$QUIET" -eq 0 ]; then
    if ask "Launch TapeXPlayer now?"; then
        open -a "${APP_NAME}"
    fi
fi

echo ""
