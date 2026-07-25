#!/bin/bash
# bundle_libs.sh - Copy and patch libraries for macOS App Bundle
# Usage: ./bundle_libs.sh <executable> <frameworks_dir> <homebrew_path>

set -e

EXECUTABLE="$1"
FRAMEWORKS_DIR="$2"
HOMEBREW_PATH="$3"

if [ -z "$EXECUTABLE" ] || [ -z "$FRAMEWORKS_DIR" ] || [ -z "$HOMEBREW_PATH" ]; then
    echo "Usage: $0 <executable> <frameworks_dir> <homebrew_path>"
    exit 1
fi

echo "📦 Bundling libraries for: $EXECUTABLE"
echo "   Frameworks dir: $FRAMEWORKS_DIR"
echo "   Homebrew path: $HOMEBREW_PATH"

# Create Frameworks directory if it doesn't exist
mkdir -p "$FRAMEWORKS_DIR"

# Function to copy and patch library
process_lib() {
    local lib_path="$1"
    local lib_name=$(basename "$lib_path")

    # Skip system libraries
    if [[ "$lib_path" == /System/* ]] || [[ "$lib_path" == /usr/lib/* ]]; then
        return
    fi

    # Skip already processed (check if file exists)
    if [ -f "$FRAMEWORKS_DIR/$lib_name" ]; then
        return
    fi

    # If @rpath, resolve to an actual file.
    # Homebrew installs many libs as keg-only under /opt/homebrew/opt/<pkg>/lib/
    # (e.g. /opt/homebrew/opt/ffmpeg/lib/libavcodec.62.dylib) rather than the
    # top-level /opt/homebrew/lib/.  Search in this order:
    #   1. $HOMEBREW_PATH/lib/            (standard linked path)
    #   2. Already-copied Frameworks dir  (another lib already pulled it in)
    #   3. Recursive search in $HOMEBREW_PATH/opt/  (keg-only packages)
    if [[ "$lib_path" == @rpath/* ]]; then
        lib_name="${lib_path#@rpath/}"
        if [ -f "$HOMEBREW_PATH/lib/$lib_name" ]; then
            lib_path="$HOMEBREW_PATH/lib/$lib_name"
        elif [ -f "$FRAMEWORKS_DIR/$lib_name" ]; then
            # Already in Frameworks — nothing to copy, just patch the reference
            return
        else
            local keg_found
            keg_found=$(find "$HOMEBREW_PATH/opt" -name "$lib_name" -type f 2>/dev/null | head -1)
            if [ -n "$keg_found" ]; then
                lib_path="$keg_found"
            else
                lib_path="$HOMEBREW_PATH/lib/$lib_name"  # will warn as not found below
            fi
        fi
    fi

    # If @loader_path, search relative to executable
    if [[ "$lib_path" == @loader_path/* ]]; then
        local rel_path="${lib_path#@loader_path/}"
        lib_path="$(dirname "$EXECUTABLE")/$rel_path"
    fi

    # Check file existence
    if [ ! -f "$lib_path" ]; then
        echo "   ⚠️  Skipping $lib_name (not found: $lib_path)"
        return
    fi

    echo "   📄 Copying: $lib_name"

    # Copy library
    cp "$lib_path" "$FRAMEWORKS_DIR/$lib_name"
    chmod +w "$FRAMEWORKS_DIR/$lib_name"

    # Patch library id
    install_name_tool -id "@executable_path/../Frameworks/$lib_name" "$FRAMEWORKS_DIR/$lib_name" 2>/dev/null || true

    # Get dependencies of this library
    local deps=$(otool -L "$lib_path" | tail -n +2 | awk '{print $1}')

    # Recursively process dependencies
    for dep in $deps; do
        process_lib "$dep"

        # Patch dependency reference
        local dep_name=$(basename "$dep")
        if [ -f "$FRAMEWORKS_DIR/$dep_name" ]; then
            install_name_tool -change "$dep" "@executable_path/../Frameworks/$dep_name" "$FRAMEWORKS_DIR/$lib_name" 2>/dev/null || true
        fi
    done
}

# Get list of executable dependencies
echo ""
echo "🔍 Analyzing dependencies..."
LIBS=$(otool -L "$EXECUTABLE" | tail -n +2 | awk '{print $1}')

# Process each library
for lib in $LIBS; do
    process_lib "$lib"
done

# Patch executable to use @executable_path
echo ""
echo "🔧 Patching executable..."
for lib in $LIBS; do
    lib_name=$(basename "$lib")

    # Skip system libraries
    if [[ "$lib" == /System/* ]] || [[ "$lib" == /usr/lib/* ]]; then
        continue
    fi

    if [ -f "$FRAMEWORKS_DIR/$lib_name" ]; then
        echo "   🔗 $lib_name -> @executable_path/../Frameworks/$lib_name"
        install_name_tool -change "$lib" "@executable_path/../Frameworks/$lib_name" "$EXECUTABLE" 2>/dev/null || true
    fi
done

# Add @executable_path/../Frameworks to rpath
install_name_tool -add_rpath "@executable_path/../Frameworks" "$EXECUTABLE" 2>/dev/null || true

# ── Second pass: patch inter-library references inside Frameworks ─────────────
# The first pass only patches the main binary's references.
# But dylibs in Frameworks reference each other using absolute Homebrew paths
# (e.g. libavformat.61.dylib → /opt/homebrew/Cellar/ffmpeg@7/.../libavcodec.61.dylib).
# Those absolute paths don't exist on the end user's machine.
# This pass reads each COPY in Frameworks and patches any remaining absolute refs.
echo ""
echo "🔧 Patching inter-library references in Frameworks..."
PATCHED_COUNT=0
for dylib in "$FRAMEWORKS_DIR"/*.dylib; do
    [ -f "$dylib" ] || continue
    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        dep_name=$(basename "$dep")
        # Skip @-paths and system libs — only absolute paths need fixing
        if [[ "$dep" == @* ]] || [[ "$dep" == /System/* ]] || [[ "$dep" == /usr/lib/* ]]; then
            continue
        fi
        # If the referenced lib is in our Frameworks, patch to @loader_path
        if [ -f "$FRAMEWORKS_DIR/$dep_name" ]; then
            install_name_tool -change "$dep" "@loader_path/$dep_name" "$dylib" 2>/dev/null && \
                PATCHED_COUNT=$((PATCHED_COUNT + 1)) || true
        fi
    done < <(otool -L "$dylib" 2>/dev/null | tail -n +2 | awk '{print $1}' || true)
done
echo "   Inter-library references patched: $PATCHED_COUNT"

# ── Safety assertion: the app is SDL2-only ───────────────────────────────────
# Homebrew has been migrating sdl2_ttf/ffmpeg to link SDL3. If any of that leaks
# into the bundle (or the main binary), the shipped .app crashes at launch with
# an SDL3-not-found error. Fail the bundling step rather than ship a crasher.
echo ""
echo "🔎 Verifying no SDL3 contamination in the bundle..."
SDL3_HITS=0
if otool -L "$EXECUTABLE" 2>/dev/null | grep -qi 'libSDL3'; then
    echo "   ❌ main binary links libSDL3:"
    otool -L "$EXECUTABLE" | grep -i sdl || true
    SDL3_HITS=$((SDL3_HITS + 1))
fi
for dylib in "$FRAMEWORKS_DIR"/*.dylib; do
    [ -f "$dylib" ] || continue
    if otool -L "$dylib" 2>/dev/null | grep -qi 'libSDL3'; then
        echo "   ❌ $(basename "$dylib") links libSDL3"
        SDL3_HITS=$((SDL3_HITS + 1))
    fi
done
# Also catch a stray libSDL3 dylib copied into Frameworks.
if ls "$FRAMEWORKS_DIR"/libSDL3* >/dev/null 2>&1; then
    echo "   ❌ libSDL3 dylib present in Frameworks: $(ls "$FRAMEWORKS_DIR"/libSDL3*)"
    SDL3_HITS=$((SDL3_HITS + 1))
fi
# sdl2-compat evades the otool checks above: it is an SDL2-ABI shim that dlopen()s SDL3
# at RUNTIME, so it carries NO load-time libSDL3 reference — yet the app still needs SDL3
# present at launch, which we don't ship → crash on a clean Mac. Detect it by its strings.
for dylib in "$EXECUTABLE" "$FRAMEWORKS_DIR"/libSDL2-*.dylib; do
    [ -f "$dylib" ] || continue
    if strings -a "$dylib" 2>/dev/null | grep -qiE 'sdl2-compat|Failed loading SDL3 library'; then
        echo "   ❌ $(basename "$dylib") is sdl2-compat (an SDL3 shim), not real SDL2"
        SDL3_HITS=$((SDL3_HITS + 1))
    fi
done
if [ "$SDL3_HITS" -ne 0 ]; then
    echo ""
    echo "❌  SDL3 contamination detected ($SDL3_HITS). This app is SDL2-only."
    echo "    Fix the Homebrew deps (see .github/workflows/macos-build.yml guard)."
    exit 1
fi
echo "   ✅ Bundle is SDL2-clean."

# Count copied libraries
LIB_COUNT=$(find "$FRAMEWORKS_DIR" -name "*.dylib" 2>/dev/null | wc -l | tr -d ' ')

echo ""
echo "✅ Library bundling complete!"
echo "   Copied libraries: $LIB_COUNT"
