#!/bin/bash
# Update BuildInfo.h with current version and build number

VERSION_FILE="../VERSION"
BUILD_NUMBER_FILE=".build_number"
OUTPUT_FILE="modules/FSTPMainModule/WSGUI/linux/BuildInfo.h"

# Read version
if [ -f "$VERSION_FILE" ]; then
    source "$VERSION_FILE"
else
    VERSION="2026.01"
    CODE_NAME="Albatross"
fi

# Read build number
if [ -f "$BUILD_NUMBER_FILE" ]; then
    BUILD_NUMBER=$(cat "$BUILD_NUMBER_FILE")
else
    BUILD_NUMBER="0"
fi

# Generate BuildInfo.h
cat > "$OUTPUT_FILE" << EOF
#pragma once

// Auto-generated build information for Linux
// Generated on: $(date)
// This file is synchronized with ../VERSION and .build_number

#define TAPEXPLAYER_VERSION "${VERSION}"
#define TAPEXPLAYER_CODE_NAME "${CODE_NAME}"
#define TAPEXPLAYER_BUILD_NUMBER "${BUILD_NUMBER}"
#define TAPEXPLAYER_BUILD_DATE __DATE__

// Helper functions
inline const char* GetTapeXPlayerVersion() {
    return TAPEXPLAYER_VERSION;
}

inline const char* GetTapeXPlayerCodeName() {
    return TAPEXPLAYER_CODE_NAME;
}

inline const char* GetTapeXPlayerBuildNumber() {
    return TAPEXPLAYER_BUILD_NUMBER;
}

inline const char* GetTapeXPlayerBuildDate() {
    return TAPEXPLAYER_BUILD_DATE;
}
EOF

echo "✅ BuildInfo.h updated: ${VERSION} \"${CODE_NAME}\" Build ${BUILD_NUMBER}"
