# TapeXPlayer 2026 Makefile
# Automated build using wildcard
#
# MAIN TARGETS:
#   make          - Build executable
#   make bundle   - Create .app bundle with libraries (current architecture)
#   make bundle-universal - Create Universal Binary + auto-install to /Applications (x86_64 + arm64)
#   make notarize - Notarize application for public distribution (requires Developer ID)
#   make app      - Create basic .app bundle (requires installed Homebrew dependencies)
#   make clean    - Clean object files
#   make rebuild  - Rebuild everything from scratch
#
# OPTIONS:
#   SKIP_INSTALL=1 - Skip automatic installation to /Applications (for bundle-universal only)
#     Example: make bundle-universal SKIP_INSTALL=1
#

# === BUILD VERSION CONFIGURATION ===
# Application version (can override: make VERSION=2026.02)
VERSION ?= 2026.01

# Build code name (can override: make CODE_NAME=Apollo)
CODE_NAME ?= Albatross

# File for storing build number
BUILD_NUMBER_FILE = .build_number

# Automatic build number increment on each make invocation
BUILD_NUMBER := $(shell \
	CURRENT=$$(cat $(BUILD_NUMBER_FILE) 2>/dev/null || echo "852"); \
	NEXT=$$((CURRENT + 1)); \
	echo $$NEXT > $(BUILD_NUMBER_FILE); \
	echo $$NEXT \
)

# Automatic build date generation
BUILD_DATE := $(shell date +"%d/%m/%Y")

# Path to generated BuildInfo.swift file
BUILD_INFO_SWIFT = modules/FSTPMainModule/WSGUI/darwin/sdl/BuildInfo.swift

# Compiler and flags
CXX = g++
SWIFTC = swiftc

# Automatic architecture detection
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Platform detection and configuration
ifneq (,$(findstring NT-,$(UNAME_S)))
    # Windows (MSYS2/MinGW) configuration
    PLATFORM = windows
    BUILD_TYPE = Windows (SDL2/MinGW)
    ARCH_FLAGS =
    
    # Windows-specific paths and flags
    MINGW_PATH = /mingw64
    HOMEBREW_PATH = $(MINGW_PATH)
    
    # Output directories
    OBJ_DIR = ../builds/obj_windows
    BIN_DIR = ../builds/binaries
    TARGET = $(BIN_DIR)/TapeXPlayer.exe
    
    # Disable macOS-specific settings
    BUILD_INFO_SWIFT =
    SWIFT_TARGET =
    
    # Windows compilation flags
    CXXFLAGS = -std=c++23 -Wall -Wextra -O2 -g -msse2 -msse4.1 -mavx -mavx2 -Wno-unused-parameter
    INCLUDES = -I$(MINGW_PATH)/include -I$(MINGW_PATH)/include/SDL2 \
              -Imodules/FSTPMainModule -Imodules/FSTPMainModule/WSGUI \
              -Imodules/FSTPPlayerModule -Imodules/FSTPVideoModule \
              -I$(MINGW_PATH)/include/ffmpeg
    LDFLAGS = -L$(MINGW_PATH)/lib
    LIBS = -lSDL2 -lportaudio -lwinmm -lole32 -lgdi32
    
    # Include paths
    INCLUDES = -I$(MINGW_PATH)/include \
              -I$(MINGW_PATH)/include/SDL2 \
              -Imodules/FSTPMainModule \
              -Imodules/FSTPMainModule/WSGUI \
              -Imodules/FSTPPlayerModule \
              -Imodules/FSTPVideoModule
    
    # Compiler flags
    CXXFLAGS = -std=c++23 -Wall -Wextra -O2 -g \
               -msse2 -msse4.1 -mavx -mavx2 \
               -Wno-unused-parameter \
               $(INCLUDES)
    
    # Library paths and libs
    LDFLAGS = -L$(MINGW_PATH)/lib
    LIBS = -lSDL2 -lportaudio -lwinmm -lole32 -lgdi32
    
    # Output directories
    OBJ_DIR = ../builds/obj_windows
    BIN_DIR = ../builds/binaries
    TARGET = $(BIN_DIR)/TapeXPlayer.exe

else ifeq ($(UNAME_S),Linux)
    # Linux - no architecture flags needed for g++
    ARCH_FLAGS =
    HOMEBREW_PATH = /usr/local
else ifeq ($(UNAME_M),arm64)
    # M1/M2 Mac - use /opt/homebrew
    ARCH_FLAGS = -arch arm64
    HOMEBREW_PATH = /opt/homebrew
else
    # Intel Mac - use /usr/local
    ifeq ($(PLATFORM),windows)
        ARCH_FLAGS =
    else
        ARCH_FLAGS = -arch x86_64
    endif
    HOMEBREW_PATH = /usr/local
endif

# Base flags - platform-specific SIMD flags set below
CXXFLAGS_BASE = -std=c++23 -Wall -Wextra $(ARCH_FLAGS)
OBJCFLAGS = -std=c++23 -Wall -Wextra $(ARCH_FLAGS)

# Platform-specific configuration
ifeq ($(UNAME_S),Linux)
    # Linux configuration
    SWIFT_TARGET =
    SWIFTFLAGS =
    USE_SWIFT = no
    SDL_PATH = /usr

    # REMOVED AVX512 due to heap corruption issues on Intel CPUs
    # AVX512 requires 64-byte alignment which causes problems with AVFrame allocation
    CXXFLAGS = -std=c++23 -Wall -Wextra -O2 -g -march=x86-64-v3 -msse2 -msse4.1 -mavx -mavx2 -Wno-unused-parameter

    # GTK+ flags for file dialogs
    GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0)
    GTK_LIBS = $(shell pkg-config --libs gtk+-3.0)

    # Include paths
    INCLUDES = -I/usr/include/SDL2 \
               -Imodules/FSTPMainModule \
               -Imodules/FSTPMainModule/WSGUI \
               -Imodules/FSTPPlayerModule \
               -Imodules/FSTPVideoModule \
               -Imodules/FSTPAudioModule \
               $(GTK_CFLAGS)

    # Library flags
    LIBS = -lSDL2 -lSDL2_ttf -lportaudio \
           -lavformat -lavcodec -lavutil -lswscale \
           -lssl -lcrypto -lrtmidi \
           -lpthread -ldl -lm \
           $(GTK_LIBS)

    LINKER = $(CXX)
    OBJDIR = ../builds/obj_linux
    TARGET = ../builds/binaries/TapeXPlayer_linux
else
    # macOS configuration
    USE_SWIFT = yes
    # Swift target determined by architecture (requires macOS 13.0 for modern SwiftUI APIs)
    ifeq ($(UNAME_M),arm64)
        SWIFT_TARGET = arm64-apple-macosx13.0
        # Apple Silicon - use NEON for SIMD (ARM architecture)
        CXXFLAGS = $(CXXFLAGS_BASE) -O2 -g -Wno-unused-parameter
    else
        SWIFT_TARGET = x86_64-apple-macosx13.0
        # Intel Mac - use AVX2 for SIMD optimizations (same as Linux)
        # REMOVED AVX512 due to heap corruption issues on Intel CPUs
        CXXFLAGS = $(CXXFLAGS_BASE) -O2 -g -msse2 -msse4.1 -mavx -mavx2 -Wno-unused-parameter
    endif
    SWIFTFLAGS = -target $(SWIFT_TARGET) -import-objc-header modules/FSTPMainModule/WSGUI/darwin/sdl/TapeXPlayer-Bridging-Header.h -parse-as-library
    SDL_PATH = $(HOMEBREW_PATH)
    INCLUDES = -I$(SDL_PATH)/include -Imodules/FSTPMainModule -Imodules/FSTPMainModule/WSGUI -Imodules/FSTPPlayerModule -Imodules/FSTPVideoModule
    LIBS = -L$(SDL_PATH)/lib -lSDL2 -lSDL2_ttf -lportaudio -lavformat -lavcodec -lavutil -lswscale -lssl -lcrypto -lrtmidi -framework Cocoa -framework CoreFoundation -framework CoreVideo -framework UniformTypeIdentifiers -framework VideoToolbox -framework IOKit -framework QuartzCore -framework CoreMIDI
    LINKER = $(SWIFTC)
    LINKER_FLAGS = $(SWIFT_TARGET:%= -target %) -Xlinker -lc++
    OBJDIR = ../builds/obj
    TARGET = ../builds/binaries/TapeXPlayer
endif
LIBS_WITH_ARCH = $(LIBS) $(ARCH_FLAGS)
# TEMPORARILY REMOVED: -framework Metal -framework MetalKit (causes crashes with software rendering)

# Build folders (platform-specific paths set in conditional blocks above)
# OBJDIR and TARGET are already set in platform-specific sections
APP_BUNDLE ?= ../builds/TapeXPlayer.app
ICON_SOURCE = ../resources/TapeXPlayer.icns

# Integrated test
TEST_TARGET = ../builds/binaries/test_integrated_system

# Render test
RENDER_TEST_TARGET = ../builds/binaries/test_render_backends

# Find source files with wildcard (exclude example and _old files)
CPP_SOURCES_RAW = $(filter-out modules/FSTPPlayerModule/example_usage.cpp, \
                  $(filter-out modules/FSTPVideoModule/FSTPVideoTextureInterface_example_usage.cpp, \
                  $(filter-out %_old.cpp, \
                  $(wildcard modules/*/*.cpp) \
                  $(wildcard modules/*/*/*.cpp) \
                  $(wildcard modules/*/*/*/*.cpp) \
                  $(wildcard modules/*/*/*/*/*.cpp))))

# Platform-specific source filtering
ifeq ($(UNAME_S),Linux)
    # Exclude macOS-specific files (darwin and windows directories)
    CPP_SOURCES = $(filter-out modules/FSTPMainModule/WSGUI/darwin/%, \
                  $(filter-out modules/FSTPMainModule/WSGUI/windows/%, \
                  $(CPP_SOURCES_RAW)))
    MM_SOURCES =
    SWIFT_SOURCES =
else
    # macOS - include all files
    CPP_SOURCES = $(CPP_SOURCES_RAW)
    MM_SOURCES = $(wildcard modules/*/*.mm) $(wildcard modules/*/*/*.mm) $(wildcard modules/*/*/*/*.mm) $(wildcard modules/*/*/*/*/*.mm)
    # Exclude BuildInfo.swift from scanning (it's auto-generated)
    SWIFT_SOURCES = $(filter-out $(BUILD_INFO_SWIFT), $(wildcard modules/*/*/*/*.swift) $(wildcard modules/*/*/*/*/*.swift))
endif

# Object files in separate folder
CPP_OBJECTS = $(CPP_SOURCES:%.cpp=$(OBJDIR)/%.o)
MM_OBJECTS = $(MM_SOURCES:%.mm=$(OBJDIR)/%.o)
# Swift compiled as single module (not individual files) - only on macOS
SWIFT_MODULE_OBJECT = $(OBJDIR)/SwiftModule.o

ifeq ($(USE_SWIFT),yes)
    OBJECTS = $(CPP_OBJECTS) $(MM_OBJECTS) $(SWIFT_MODULE_OBJECT)
else
    OBJECTS = $(CPP_OBJECTS) $(MM_OBJECTS)
endif

# Main target
all: platform-info $(TARGET)

# Platform information
.PHONY: platform-info
platform-info:
	@echo "================================================"
	@echo "TapeXPlayer 2026 - Universal Build System"
	@echo "================================================"
	@echo "Platform:        $(UNAME_S)"
	@echo "Architecture:    $(UNAME_M)"
ifeq ($(UNAME_S),Linux)
	@echo "Target:          $(TARGET)"
	@echo "Build Type:      Linux (GTK+/SDL2)"
	@echo "Compiler:        $(CXX)"
	@echo "Flags:           $(CXXFLAGS)"
	@echo "------------------------------------------------"
else
	@echo "Target:          $(TARGET)"
	@echo "Build Type:      macOS (Cocoa/SDL2/Swift)"
	@echo "Architecture:    $(ARCH_FLAGS)"
	@echo "Swift Target:    $(SWIFT_TARGET)"
	@echo "Homebrew Path:   $(HOMEBREW_PATH)"
	@echo "Build Number:    $(BUILD_NUMBER)"
	@echo "Version:         $(VERSION)"
	@echo "Code Name:       $(CODE_NAME)"
	@echo "------------------------------------------------"
endif
	@echo ""

# Generate BuildInfo.swift before build (always regenerated) - only on macOS
ifeq ($(USE_SWIFT),yes)
.PHONY: $(BUILD_INFO_SWIFT)
$(BUILD_INFO_SWIFT):
	@echo "Generating BuildInfo.swift (Version: $(VERSION), Build: $(BUILD_NUMBER), Date: $(BUILD_DATE))..."
	@mkdir -p $(dir $(BUILD_INFO_SWIFT))
	@echo "// Auto-generated by Makefile - DO NOT EDIT MANUALLY" > $(BUILD_INFO_SWIFT)
	@echo "// Generated on: $$(date)" >> $(BUILD_INFO_SWIFT)
	@echo "" >> $(BUILD_INFO_SWIFT)
	@echo "import Foundation" >> $(BUILD_INFO_SWIFT)
	@echo "" >> $(BUILD_INFO_SWIFT)
	@echo "@available(macOS 13.0, *)" >> $(BUILD_INFO_SWIFT)
	@echo "struct BuildInfo {" >> $(BUILD_INFO_SWIFT)
	@echo "    static let version = \"$(VERSION)\"" >> $(BUILD_INFO_SWIFT)
	@echo "    static let buildNumber = \"$(BUILD_NUMBER)\"" >> $(BUILD_INFO_SWIFT)
	@echo "    static let buildDate = \"$(BUILD_DATE)\"" >> $(BUILD_INFO_SWIFT)
	@if [ -n "$(CODE_NAME)" ]; then \
		echo "    static let codeName = \"$(CODE_NAME)\"" >> $(BUILD_INFO_SWIFT); \
	else \
		echo "    static let codeName: String? = nil" >> $(BUILD_INFO_SWIFT); \
	fi
	@echo "}" >> $(BUILD_INFO_SWIFT)
	@echo "BuildInfo.swift generated."

$(TARGET): $(BUILD_INFO_SWIFT) $(OBJECTS)
	@echo "Linking $(TARGET)..."
	@mkdir -p $(dir $(TARGET))
	$(LINKER) $(LINKER_FLAGS) $(OBJECTS) $(LIBS) -o $(TARGET)
	@echo "Build complete!"
else
# Linux build - update BuildInfo.h before building
.PHONY: update-buildinfo
update-buildinfo:
	@chmod +x update_buildinfo.sh
	@./update_buildinfo.sh

$(TARGET): update-buildinfo $(OBJECTS)
	@echo "Linking $(TARGET)..."
	@mkdir -p $(dir $(TARGET))
	$(LINKER) $(OBJECTS) $(LIBS) -o $(TARGET)
	@echo ""
	@echo "================================================"
	@echo "✅ Build complete!"
	@echo "================================================"
	@echo "Target:     $(TARGET)"
	@echo "Platform:   Linux ($(UNAME_M))"
	@echo "Files:      $(words $(CPP_SOURCES)) C++ sources"
	@echo "Version:    $(shell grep 'TAPEXPLAYER_VERSION' modules/FSTPMainModule/WSGUI/linux/BuildInfo.h | cut -d'\"' -f2) Build $(shell grep 'TAPEXPLAYER_BUILD_NUMBER' modules/FSTPMainModule/WSGUI/linux/BuildInfo.h | cut -d'\"' -f2)"
	@echo ""
	@echo "To run: $(TARGET)"
	@echo "================================================"
endif

# Compile C++ files
$(OBJDIR)/%.o: %.cpp
	@echo "Compiling $<..."
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Compile Objective-C++ files
$(OBJDIR)/%.o: %.mm
	@echo "Compiling $<..."
	@mkdir -p $(dir $@)
	$(CXX) $(OBJCFLAGS) $(INCLUDES) -c $< -o $@

# Compile Swift module (all files together + BuildInfo.swift) - only on macOS
ifeq ($(USE_SWIFT),yes)
.PHONY: force_swift_rebuild
$(SWIFT_MODULE_OBJECT): force_swift_rebuild $(BUILD_INFO_SWIFT) $(SWIFT_SOURCES)
	@echo "Compiling Swift module (BuildInfo + $(words $(SWIFT_SOURCES)) files)..."
	@mkdir -p $(dir $@)
	@rm -f $@
	$(SWIFTC) $(SWIFTFLAGS) $(INCLUDES) -whole-module-optimization -emit-object $(BUILD_INFO_SWIFT) $(SWIFT_SOURCES) -o $@

force_swift_rebuild:
endif

# Clean
clean:
	@echo "Cleaning build artifacts..."
ifneq (,$(findstring MSYS_NT,$(UNAME_S)))
	@echo "Cleaning Windows build..."
	rm -rf ../builds/obj_windows
	rm -f ../builds/binaries/TapeXPlayer.exe
	rm -f modules/FSTPMainModule/WSGUI/windows/BuildInfo.h
else ifeq ($(UNAME_S),Linux)
	@echo "Cleaning Linux build..."
	rm -rf ../builds/obj_linux
	rm -f ../builds/binaries/TapeXPlayer_linux
	rm -f modules/FSTPMainModule/WSGUI/linux/BuildInfo.h
else
	@echo "Cleaning macOS build..."
	rm -rf ../builds/obj
	rm -f ../builds/binaries/TapeXPlayer
	rm -f $(BUILD_INFO_SWIFT)
	rm -rf ../builds/TapeXPlayer*.app
endif
	@echo "✅ Clean complete"

# Rebuild
rebuild: clean all

# Run
run: $(TARGET)
	@echo "Running TapeXPlayer..."
	$(TARGET)


# === CREATE APP BUNDLE FOR macOS ===

app: $(TARGET)
	@echo "Creating macOS App Bundle..."
	@rm -rf $(APP_BUNDLE)
	@mkdir -p $(APP_BUNDLE)/Contents/MacOS
	@mkdir -p $(APP_BUNDLE)/Contents/Resources
	@echo "Copying executable..."
	@cp $(TARGET) $(APP_BUNDLE)/Contents/MacOS/TapeXPlayer
	@echo "Copying icon..."
	@if [ -f $(ICON_SOURCE) ]; then \
		cp $(ICON_SOURCE) $(APP_BUNDLE)/Contents/Resources/TapeXPlayer.icns; \
	else \
		echo "WARNING: Icon not found at $(ICON_SOURCE)"; \
	fi
	@echo "Creating Info.plist with build number $(BUILD_NUMBER)..."
	@if [ -n "$(CODE_NAME)" ]; then \
		sed "s/SHORT_VERSION_PLACEHOLDER/$(CODE_NAME) (Build $(BUILD_NUMBER))/g; s/BUILD_NUMBER_PLACEHOLDER/$(BUILD_NUMBER)/g" Info.plist > $(APP_BUNDLE)/Contents/Info.plist; \
	else \
		sed "s/SHORT_VERSION_PLACEHOLDER/$(BUILD_NUMBER)/g; s/BUILD_NUMBER_PLACEHOLDER/$(BUILD_NUMBER)/g" Info.plist > $(APP_BUNDLE)/Contents/Info.plist; \
	fi
	@echo "App Bundle created: $(APP_BUNDLE)"
	@echo ""
	@echo "To run use: open $(APP_BUNDLE)"

# === BUNDLE WITH PACKAGED LIBRARIES ===

bundle: $(TARGET)
	@echo "Creating self-contained macOS App Bundle..."
	@rm -rf $(APP_BUNDLE)
	@mkdir -p $(APP_BUNDLE)/Contents/MacOS
	@mkdir -p $(APP_BUNDLE)/Contents/Resources
	@mkdir -p $(APP_BUNDLE)/Contents/Frameworks
	@echo "Copying executable..."
	@cp $(TARGET) $(APP_BUNDLE)/Contents/MacOS/TapeXPlayer
	@echo "Copying icon..."
	@if [ -f $(ICON_SOURCE) ]; then \
		cp $(ICON_SOURCE) $(APP_BUNDLE)/Contents/Resources/TapeXPlayer.icns; \
	else \
		echo "WARNING: Icon not found at $(ICON_SOURCE)"; \
	fi
	@echo "Creating Info.plist..."
	@if [ -n "$(CODE_NAME)" ]; then \
		sed "s/SHORT_VERSION_PLACEHOLDER/$(CODE_NAME) (Build $(BUILD_NUMBER))/g; s/BUILD_NUMBER_PLACEHOLDER/$(BUILD_NUMBER)/g" Info.plist > $(APP_BUNDLE)/Contents/Info.plist; \
	else \
		sed "s/SHORT_VERSION_PLACEHOLDER/$(BUILD_NUMBER)/g; s/BUILD_NUMBER_PLACEHOLDER/$(BUILD_NUMBER)/g" Info.plist > $(APP_BUNDLE)/Contents/Info.plist; \
	fi
	@echo "Copying and patching libraries..."
	@chmod +x bundle_libs.sh
	@./bundle_libs.sh $(APP_BUNDLE)/Contents/MacOS/TapeXPlayer $(APP_BUNDLE)/Contents/Frameworks $(HOMEBREW_PATH)
	@echo "Signing application (ad-hoc)..."
	@codesign -s - --force --deep $(APP_BUNDLE) 2>/dev/null || echo "⚠️  Code signing failed (not critical)"
	@echo ""
	@echo "✅ Self-contained App Bundle created: $(APP_BUNDLE)"
	@echo "To run use: open $(APP_BUNDLE)"

# === UNIVERSAL BINARY (x86_64 + arm64) ===

bundle-universal:
	@echo "Building Universal Binary (x86_64 + arm64)..."
	@echo ""
	@echo "1️⃣  Building for arm64..."
	@rm -rf ../builds/obj
	@rm -f ../builds/binaries/TapeXPlayer
	@rm -f $(BUILD_INFO_SWIFT)
	@APP_BUNDLE=../builds/TapeXPlayer_arm64.app $(MAKE) bundle ARCH_FLAGS="-arch arm64" HOMEBREW_PATH=/opt/homebrew SWIFT_TARGET=arm64-apple-macosx13.0
	@echo ""
	@echo "2️⃣  Building for x86_64..."
	@rm -rf ../builds/obj
	@rm -f ../builds/binaries/TapeXPlayer
	@rm -f $(BUILD_INFO_SWIFT)
	@APP_BUNDLE=../builds/TapeXPlayer_x86_64.app $(MAKE) bundle ARCH_FLAGS="-arch x86_64" HOMEBREW_PATH=/usr/local SWIFT_TARGET=x86_64-apple-macosx13.0
	@echo ""
	@echo "3️⃣  Creating Universal Binary..."
	@rm -rf $(APP_BUNDLE)
	@cp -R ../builds/TapeXPlayer_arm64.app $(APP_BUNDLE)
	@lipo -create ../builds/TapeXPlayer_arm64.app/Contents/MacOS/TapeXPlayer \
	             ../builds/TapeXPlayer_x86_64.app/Contents/MacOS/TapeXPlayer \
	             -output $(APP_BUNDLE)/Contents/MacOS/TapeXPlayer
	@echo "4️⃣  Merging Frameworks..."
	@chmod +x merge_universal_frameworks.sh
	@./merge_universal_frameworks.sh ../builds/TapeXPlayer_arm64.app/Contents/Frameworks \
	                                  ../builds/TapeXPlayer_x86_64.app/Contents/Frameworks \
	                                  $(APP_BUNDLE)/Contents/Frameworks
	@echo "5️⃣  Signing Universal Binary (ad-hoc)..."
	@codesign -s - --force --deep $(APP_BUNDLE) 2>/dev/null || echo "⚠️  Code signing failed (not critical)"
	@echo ""
ifndef SKIP_INSTALL
	@echo "6️⃣  Installing to /Applications..."
	@if [ -d "/Applications/TapeXPlayer.app" ]; then \
		echo "   Removing old version from /Applications..."; \
		rm -rf /Applications/TapeXPlayer.app; \
	fi
	@cp -R $(APP_BUNDLE) /Applications/
	@echo "   ✅ Installed to /Applications/TapeXPlayer.app"
	@echo ""
	@echo "✅ Universal Binary created: $(APP_BUNDLE)"
	@echo "✅ Installed to /Applications"
	@echo "Supported architectures:"
	@lipo -info $(APP_BUNDLE)/Contents/MacOS/TapeXPlayer
	@echo ""
	@echo "To run use: open -a TapeXPlayer"
else
	@echo "✅ Universal Binary created: $(APP_BUNDLE)"
	@echo "⏭  Auto-install skipped (SKIP_INSTALL=1)"
	@echo "Supported architectures:"
	@lipo -info $(APP_BUNDLE)/Contents/MacOS/TapeXPlayer
	@echo ""
	@echo "To run use: open $(APP_BUNDLE)"
endif


# === GITHUB RELEASE (WITHOUT NOTARIZATION) ===

release: $(APP_BUNDLE)
	@echo "📦 Creating GitHub Release..."
	@chmod +x create_release.sh
	@./create_release.sh $(BUILD_NUMBER) $(CODE_NAME)

release-help:
	@echo "Creating GitHub Release archive"
	@echo ""
	@echo "Usage:"
	@echo "  make bundle        # First build the bundle"
	@echo "  make release       # Creates ZIP with README and install script"
	@echo ""
	@echo "Or with custom code name:"
	@echo "  make release CODE_NAME=Albatross"
	@echo ""
	@echo "Creates ZIP archive containing:"
	@echo "  • TapeXPlayer.app (Universal Binary)"
	@echo "  • README.txt (detailed instructions)"
	@echo "  • install.command (automatic installation)"
	@echo "  • uninstall.command (removal)"
	@echo ""
	@echo "Result: ../builds/TapeXPlayer-CODENAME-buildXXX.zip"
	@echo "Ready to upload to GitHub Releases!"
	@echo "Ready for distribution! 🚀"

# Show source files (useful for debugging makefile)
.PHONY: show-sources
show-sources:
	@echo "Platform: $(UNAME_S)"
	@echo ""
	@echo "C++ Sources ($(words $(CPP_SOURCES)) files):"
	@echo "$(CPP_SOURCES)" | tr ' ' '\n' | sort
ifeq ($(UNAME_S),Darwin)
	@echo ""
	@echo "Objective-C++ Sources ($(words $(MM_SOURCES)) files):"
	@echo "$(MM_SOURCES)" | tr ' ' '\n' | sort
	@echo ""
	@echo "Swift Sources ($(words $(SWIFT_SOURCES)) files):"
	@echo "$(SWIFT_SOURCES)" | tr ' ' '\n' | sort
endif
	@echo ""
	@echo "Total files: $(words $(CPP_SOURCES) $(MM_SOURCES) $(SWIFT_SOURCES))"

# Help target for Linux users
.PHONY: help
help:
	@echo "TapeXPlayer 2026 - Universal Build System"
	@echo ""
	@echo "Detected platform: $(UNAME_S) ($(UNAME_M))"
	@echo ""
ifeq ($(UNAME_S),Linux)
	@echo "Linux targets:"
	@echo "  make              - Build executable"
	@echo "  make clean        - Clean build artifacts"
	@echo "  make rebuild      - Rebuild from scratch"
	@echo "  make run          - Build and run"
	@echo "  make deb          - Create DEB package"
	@echo "  make install-deb  - Build and install DEB package"
	@echo "  make show-sources - Show all source files"
	@echo ""
	@echo "Build output:"
	@echo "  Executable: $(TARGET)"
	@echo "  Objects:    $(OBJDIR)"
	@echo "  DEB:        ../binaries/deb/*.deb"
else
	@echo "macOS targets:"
	@echo "  make                    - Build executable"
	@echo "  make app                - Create .app bundle"
	@echo "  make bundle             - Create self-contained .app with libraries"
	@echo "  make bundle-universal   - Create Universal Binary (x86_64 + arm64)"
	@echo "  make clean              - Clean build artifacts"
	@echo "  make rebuild            - Rebuild from scratch"
	@echo "  make run                - Build and run"
	@echo "  make show-sources       - Show all source files"
	@echo ""
	@echo "Build output:"
	@echo "  Executable: $(TARGET)"
	@echo "  Objects:    $(OBJDIR)"
	@echo "  App Bundle: $(APP_BUNDLE)"
endif

# === LINUX DEB PACKAGE ===

ifeq ($(UNAME_S),Linux)
# Create DEB package
deb: $(TARGET)
	@echo "Creating DEB package..."
	@chmod +x create_deb.sh
	@./create_deb.sh

# Install DEB package
install-deb: deb
	@echo "Installing DEB package..."
	@DEB_FILE=$$(ls ../binaries/deb/*.deb 2>/dev/null | head -1); \
	if [ -n "$$DEB_FILE" ]; then \
		echo "Installing $$DEB_FILE..."; \
		sudo dpkg -i "$$DEB_FILE"; \
		echo "Installing dependencies..."; \
		sudo apt-get install -f; \
		echo "✅ TapeXPlayer installed successfully!"; \
		echo "Launch from applications menu or run: tapexplayer"; \
	else \
		echo "❌ No DEB package found. Run 'make deb' first."; \
		exit 1; \
	fi

# Uninstall package
uninstall:
	@echo "Uninstalling TapeXPlayer..."
	@sudo apt-get remove tapexplayer
	@echo "✅ TapeXPlayer uninstalled"
endif

.PHONY: all clean rebuild run test test-run debug config test-render test-render-run app bundle bundle-universal notarize notarize-help release release-help platform-info show-sources help deb install-deb uninstall