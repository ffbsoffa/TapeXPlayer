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

    # If @rpath, search in Homebrew
    if [[ "$lib_path" == @rpath/* ]]; then
        lib_name="${lib_path#@rpath/}"
        lib_path="$HOMEBREW_PATH/lib/$lib_name"
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

# Count copied libraries
LIB_COUNT=$(find "$FRAMEWORKS_DIR" -name "*.dylib" 2>/dev/null | wc -l | tr -d ' ')

echo ""
echo "✅ Library bundling complete!"
echo "   Copied libraries: $LIB_COUNT"
