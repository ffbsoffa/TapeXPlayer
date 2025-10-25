#!/bin/bash
# Quick build test script for TapeXPlayer universal build system

set -e  # Exit on error

echo "========================================"
echo "TapeXPlayer Build System Test"
echo "========================================"
echo ""

# Detect platform
PLATFORM=$(uname -s)
ARCH=$(uname -m)

echo "Platform: $PLATFORM ($ARCH)"
echo ""

# Test 1: Help
echo "Test 1: make help"
echo "----------------------------------------"
make help
echo ""

# Test 2: Show sources
echo "Test 2: make show-sources"
echo "----------------------------------------"
make show-sources | head -20
echo "..."
echo ""

# Test 3: Clean
echo "Test 3: make clean"
echo "----------------------------------------"
make clean
echo ""

# Test 4: Build
echo "Test 4: make (build)"
echo "----------------------------------------"
time make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
echo ""

# Test 5: Check executable
echo "Test 5: Check executable"
echo "----------------------------------------"
if [ "$PLATFORM" = "Linux" ]; then
    ls -lh ../builds/binaries/TapeXPlayer_linux
    echo "✅ Linux executable created successfully!"
else
    ls -lh ../builds/binaries/TapeXPlayer
    echo "✅ macOS executable created successfully!"
fi
echo ""

echo "========================================"
echo "✅ All tests passed!"
echo "========================================"
