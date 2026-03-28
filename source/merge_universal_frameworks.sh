#!/bin/bash
# merge_universal_frameworks.sh - Merge Frameworks for Universal Binary
# Usage: ./merge_universal_frameworks.sh <arm64_frameworks> <x86_64_frameworks> <output_frameworks>

set -e

ARM64_DIR="$1"
X86_64_DIR="$2"
OUTPUT_DIR="$3"

if [ -z "$ARM64_DIR" ] || [ -z "$X86_64_DIR" ] || [ -z "$OUTPUT_DIR" ]; then
    echo "Usage: $0 <arm64_frameworks> <x86_64_frameworks> <output_frameworks>"
    exit 1
fi

echo "🔀 Merging Frameworks into Universal Binary..."
echo "   arm64:   $ARM64_DIR"
echo "   x86_64:  $X86_64_DIR"
echo "   output:  $OUTPUT_DIR"
echo ""

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Get list of all dylibs from both directories
LIBS_ARM64=$(find "$ARM64_DIR" -name "*.dylib" -exec basename {} \; 2>/dev/null | sort -u)
LIBS_X86_64=$(find "$X86_64_DIR" -name "*.dylib" -exec basename {} \; 2>/dev/null | sort -u)

# Merge lists
ALL_LIBS=$(echo -e "$LIBS_ARM64\n$LIBS_X86_64" | sort -u)

# Process each library
for lib in $ALL_LIBS; do
    ARM64_LIB="$ARM64_DIR/$lib"
    X86_64_LIB="$X86_64_DIR/$lib"
    OUTPUT_LIB="$OUTPUT_DIR/$lib"

    if [ -f "$ARM64_LIB" ] && [ -f "$X86_64_LIB" ]; then
        # Both versions exist - merge
        echo "   🔀 $lib (arm64 + x86_64)"
        lipo -create "$ARM64_LIB" "$X86_64_LIB" -output "$OUTPUT_LIB"
    elif [ -f "$ARM64_LIB" ]; then
        # arm64 only
        echo "   📱 $lib (arm64 only)"
        cp "$ARM64_LIB" "$OUTPUT_LIB"
    elif [ -f "$X86_64_LIB" ]; then
        # x86_64 only
        echo "   💻 $lib (x86_64 only)"
        cp "$X86_64_LIB" "$OUTPUT_LIB"
    fi

    # Set write permissions
    chmod +w "$OUTPUT_LIB" 2>/dev/null || true
done

echo ""
echo "✅ Frameworks merging complete!"
