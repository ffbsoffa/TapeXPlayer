CXX = clang++
CXXFLAGS = -std=c++17 -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET)

# Use alternative source by default
USE_ALT_SOURCE = 1
USE_DEBUG_SOURCE = 0

# Common flags and frameworks
FRAMEWORKS = -framework VideoToolbox -framework AudioToolbox -framework CoreFoundation \
            -framework CoreVideo -framework CoreMedia -framework AppKit -framework CoreMIDI \
            -framework CoreAudio -framework UniformTypeIdentifiers -framework OpenGL \
            -framework Cocoa -framework Metal 

# Common libraries
LIBS = -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lSDL2 -lSDL2_ttf -lrtmidi \
       -lportaudio -lavcodec -lavformat -lavutil -lswscale -lswresample \
       -lssl -lcrypto -lGLEW -lavdevice 

# x86_64 settings
X86_PREFIX = /usr/local
X86_CFLAGS = -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET) \
             -isysroot $(shell xcrun --show-sdk-path) \
             -I$(shell xcrun --show-sdk-path)/System/Library/Frameworks/AppKit.framework/Headers \
             -I$(X86_PREFIX)/include \
             -I$(X86_PREFIX)/include/gstreamer-1.0 \
             -I$(X86_PREFIX)/include/glib-2.0 \
             -I$(X86_PREFIX)/lib/glib-2.0/include \
             -I$(X86_PREFIX)/include/SDL2 \
             -I$(X86_PREFIX)/Cellar/rtmidi/6.0.0/include \
             -I$(X86_PREFIX)/opt/openssl/include \
             -I$(SRC_DIR)/nfd/include \
             -I$(X86_PREFIX)/Cellar/ffmpeg/7.1.1_1/include

X86_LDFLAGS = -L$(X86_PREFIX)/lib \
              -L$(X86_PREFIX)/opt/openssl/lib \
              -L$(X86_PREFIX)/Cellar/rtmidi/6.0.0/lib \
              -L$(X86_PREFIX)/Cellar/ffmpeg/7.1.1_1/lib \
              -L$(X86_PREFIX)/Cellar/sdl2/2.30.11/lib \
              -L$(X86_PREFIX)/Cellar/sdl2_ttf/2.24.0/lib \
              -L$(X86_PREFIX)/Cellar/portaudio/19.7.0/lib \
              -Wl,-rpath,$(X86_PREFIX)/lib \
              -Wl,-rpath,$(X86_PREFIX)/opt/openssl/lib \
              -Wl,-rpath,$(X86_PREFIX)/Cellar/rtmidi/6.0.0/lib \
              -Wl,-rpath,$(X86_PREFIX)/Cellar/ffmpeg/7.1.1_1/lib \
              -Wl,-rpath,$(X86_PREFIX)/Cellar/sdl2/2.30.11/lib \
              -Wl,-rpath,$(X86_PREFIX)/Cellar/sdl2_ttf/2.24.0/lib \
              -Wl,-rpath,$(X86_PREFIX)/Cellar/portaudio/19.7.0/lib \
              -Wl,-rpath,@executable_path/../Frameworks

# ARM64 settings
ARM_PREFIX = /opt/homebrew
ARM_CFLAGS = -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET) \
             -isysroot $(shell xcrun --show-sdk-path) \
             -I$(shell xcrun --show-sdk-path)/System/Library/Frameworks/AppKit.framework/Headers \
             -I$(ARM_PREFIX)/include \
             -I$(ARM_PREFIX)/include/gstreamer-1.0 \
             -I$(ARM_PREFIX)/include/glib-2.0 \
             -I$(ARM_PREFIX)/lib/glib-2.0/include \
             -I$(ARM_PREFIX)/include/SDL2 \
             -I$(ARM_PREFIX)/Cellar/rtmidi/6.0.0/include \
             -I$(ARM_PREFIX)/opt/openssl/include \
             -I$(SRC_DIR)/nfd/include \
             -I$(ARM_PREFIX)/Cellar/ffmpeg/7.1.1_1/include

ARM_LDFLAGS = -L$(ARM_PREFIX)/lib \
              -L$(ARM_PREFIX)/opt/openssl/lib \
              -L$(ARM_PREFIX)/Cellar/rtmidi/6.0.0/lib \
              -L$(ARM_PREFIX)/Cellar/ffmpeg/7.1.1_1/lib \
              -L$(ARM_PREFIX)/Cellar/sdl2/2.30.11/lib \
              -L$(ARM_PREFIX)/Cellar/sdl2_ttf/2.24.0/lib \
              -L$(ARM_PREFIX)/Cellar/portaudio/19.7.0/lib \
              -Wl,-rpath,$(ARM_PREFIX)/lib \
              -Wl,-rpath,$(ARM_PREFIX)/opt/openssl/lib \
              -Wl,-rpath,$(ARM_PREFIX)/Cellar/rtmidi/6.0.0/lib \
              -Wl,-rpath,$(ARM_PREFIX)/Cellar/ffmpeg/7.1.1_1/lib \
              -Wl,-rpath,$(ARM_PREFIX)/Cellar/sdl2/2.30.11/lib \
              -Wl,-rpath,$(ARM_PREFIX)/Cellar/sdl2_ttf/2.24.0/lib \
              -Wl,-rpath,$(ARM_PREFIX)/Cellar/portaudio/19.7.0/lib \
              -Wl,-rpath,@executable_path/../Frameworks

ifeq ($(USE_DEBUG_SOURCE),1)
    ARM_NFD_LIB_PATH=source_debug/libnfd.a
    X86_NFD_LIB_PATH=source_debug/libnfd_x86_64.a
    SRC_DIR=source_debug
    TARGET_NAME = TapeXDebugger
else ifeq ($(USE_ALT_SOURCE),1)
    ARM_NFD_LIB_PATH=source_alt/libnfd.a
    X86_NFD_LIB_PATH=source_alt/libnfd_x86_64.a
    SRC_DIR=source_alt
    TARGET_NAME = TapeXPlayer
else
    ARM_NFD_LIB_PATH=source/libnfd.a
    X86_NFD_LIB_PATH=source/libnfd_x86_64.a
    SRC_DIR=source
    TARGET_NAME = TapeXPlayer
endif

SRCS = $(wildcard $(SRC_DIR)/*.cpp) \
       $(wildcard $(SRC_DIR)/common/*.cpp) \
       $(wildcard $(SRC_DIR)/main/*.cpp) \
       $(wildcard $(SRC_DIR)/core/*/*.cpp)

OBJC_SRCS = $(wildcard $(SRC_DIR)/*.mm) \
            $(wildcard $(SRC_DIR)/common/*.mm) \
            $(wildcard $(SRC_DIR)/main/*.mm) \
            $(wildcard $(SRC_DIR)/core/*/*.mm)

ARM_OBJS = $(patsubst $(SRC_DIR)/%.cpp,obj/arm64/%.o,$(SRCS)) \
           $(patsubst $(SRC_DIR)/%.mm,obj/arm64/%.o,$(OBJC_SRCS))
X86_OBJS = $(patsubst $(SRC_DIR)/%.cpp,obj/x86_64/%.o,$(SRCS)) \
           $(patsubst $(SRC_DIR)/%.mm,obj/x86_64/%.o,$(OBJC_SRCS))

ARM_TARGET = $(TARGET_NAME)_arm64
X86_TARGET = $(TARGET_NAME)_x86_64
UNIVERSAL_TARGET = $(TARGET_NAME)

# --- Переменные для App Bundle ---
APP_BUNDLE_DIR = $(TARGET_NAME).app
MACOS_DIR_REL = Contents/MacOS
RESOURCES_DIR_REL = Contents/Resources
FRAMEWORKS_DIR_REL = Contents/Frameworks
PLIST_FILE = source_alt/Info.plist

# List of grep patterns for dylibs to bundle. Be specific enough to match unique paths from otool.
# These are typically library base names or parts of their paths.
DYLIB_PATTERNS_TO_BUNDLE = \
		libavcodec \
		libavformat \
		libavutil \
		libswscale \
		libswresample \
		libSDL2-2.0.0 \
		libSDL2_ttf-2.0.0 \
		libportaudio \
		librtmidi \
		libssl.3 \
		libcrypto.3 \
		libGLEW

# Add ffmpeg binary location variables
X86_FFMPEG = $(X86_PREFIX)/bin/ffmpeg
ARM_FFMPEG = $(ARM_PREFIX)/bin/ffmpeg

# Define a helper function for copying ffmpeg
define copy_ffmpeg
	@echo "Copying ffmpeg to bundle..."
	if [ -f "$(X86_FFMPEG)" ] && [ -f "$(ARM_FFMPEG)" ]; then \
		echo "Creating universal ffmpeg binary..."; \
		lipo -create -output "./$(APP_BUNDLE_DIR)/$(RESOURCES_DIR_REL)/ffmpeg" "$(X86_FFMPEG)" "$(ARM_FFMPEG)"; \
	elif [ -f "$(X86_FFMPEG)" ]; then \
		echo "Copying x86_64 ffmpeg..."; \
		cp "$(X86_FFMPEG)" "./$(APP_BUNDLE_DIR)/$(RESOURCES_DIR_REL)/ffmpeg"; \
	elif [ -f "$(ARM_FFMPEG)" ]; then \
		echo "Copying arm64 ffmpeg..."; \
		cp "$(ARM_FFMPEG)" "./$(APP_BUNDLE_DIR)/$(RESOURCES_DIR_REL)/ffmpeg"; \
	else \
		echo "Warning: ffmpeg not found in either $(X86_FFMPEG) or $(ARM_FFMPEG)"; \
	fi
endef

# Define a helper function (multi-line variable) for processing each dylib
# Args: $(1)=pattern, $(2)=exec_path_in_bundle, $(3)=frameworks_dir_in_bundle
define process_dylib
  echo "--- Processing dylib dependency pattern: $(1) ---"; \
  ORIGINAL_PATH=$$(otool -L "$(2)" | grep "$(1)" | awk '{print $$1}' | head -n1); \
  echo "Found original path for $(1): $$ORIGINAL_PATH"; \
  if [ -z "$$ORIGINAL_PATH" ]; then \
    echo "Warning: Could not find original path for $(1) linked in $(TARGET_NAME). Skipping." >&2; \
  elif [ ! -f "$$ORIGINAL_PATH" ]; then \
    echo "Warning: Original dylib file $$ORIGINAL_PATH for $(1) not found on disk. Skipping." >&2; \
  else \
    BUNDLED_NAME=$$(basename "$$ORIGINAL_PATH"); \
    echo "Copying $$ORIGINAL_PATH to $(3)/$$BUNDLED_NAME"; \
    cp "$$ORIGINAL_PATH" "$(3)/$$BUNDLED_NAME"; \
    echo "Changing dylib ID of $(3)/$$BUNDLED_NAME to @rpath/$$BUNDLED_NAME"; \
    /usr/bin/install_name_tool -id "@rpath/$$BUNDLED_NAME" "$(3)/$$BUNDLED_NAME"; \
    echo "DEBUG: About to run install_name_tool -change"; \
    echo "DEBUG:   ORIGINAL_PATH='$$ORIGINAL_PATH'"; \
    echo "DEBUG:   NEW_PATH='@rpath/$$BUNDLED_NAME'"; \
    echo "DEBUG:   EXECUTABLE='$(2)'"; \
    /usr/bin/install_name_tool -change "$$ORIGINAL_PATH" "@rpath/$$BUNDLED_NAME" "$(2)"; \
  fi
endef

.PHONY: all clean arm64 x86_64 debug bundle run

all: bundle

debug:
	$(MAKE) USE_DEBUG_SOURCE=1 bundle # Цель debug теперь тоже собирает бандл

$(UNIVERSAL_TARGET): $(ARM_TARGET) $(X86_TARGET)
	@echo "Creating universal binary $@..."
	@lipo -create -output $@ $(ARM_TARGET) $(X86_TARGET)
	@echo "Universal binary $@ created."

arm64: $(ARM_TARGET)

x86_64: $(X86_TARGET)

$(ARM_TARGET): $(ARM_OBJS)
	@echo "Linking $(ARM_TARGET)..."
	$(CXX) -arch arm64 $(ARM_OBJS) $(ARM_NFD_LIB_PATH) $(ARM_LDFLAGS) $(LIBS) $(FRAMEWORKS) -o $@
	@echo "$(ARM_TARGET) linked."

$(X86_TARGET): $(X86_OBJS)
	@echo "Linking $(X86_TARGET)..."
	$(CXX) -arch x86_64 $(X86_OBJS) $(X86_NFD_LIB_PATH) $(X86_LDFLAGS) $(LIBS) $(FRAMEWORKS) -o $@
	@echo "$(X86_TARGET) linked."

# Цель для создания .app бандла
bundle: $(UNIVERSAL_TARGET)
	@echo "--- Starting bundle creation for $(TARGET_NAME) ---"
	@echo "UNIVERSAL_TARGET = '$@'"
	@echo "APP_BUNDLE_DIR   = './$(APP_BUNDLE_DIR)'"
	@echo "PLIST_FILE       = '$(PLIST_FILE)'"
	
	rm -rf "./$(APP_BUNDLE_DIR)"
	mkdir -p "./$(APP_BUNDLE_DIR)/$(MACOS_DIR_REL)"
	mkdir -p "./$(APP_BUNDLE_DIR)/$(RESOURCES_DIR_REL)"
	mkdir -p "./$(APP_BUNDLE_DIR)/$(FRAMEWORKS_DIR_REL)"
	
	cp "$(UNIVERSAL_TARGET)" "./$(APP_BUNDLE_DIR)/$(MACOS_DIR_REL)/$(TARGET_NAME)"
	
	if [ -f "$(PLIST_FILE)" ]; then \
		echo "Copying Info.plist from $(PLIST_FILE)..."; \
		cp "$(PLIST_FILE)" "./$(APP_BUNDLE_DIR)/Contents/Info.plist"; \
	else \
		echo "Error: PLIST_FILE '$(PLIST_FILE)' not found for $(TARGET_NAME)!" >&2; \
		exit 1; \
	fi

	@echo "--- Starting dylib processing ---"; \
	_SHELL_CURRENT_EXEC_PATH_=./$(APP_BUNDLE_DIR)/$(MACOS_DIR_REL)/$(TARGET_NAME); \
	_SHELL_CURRENT_FW_PATH_=./$(APP_BUNDLE_DIR)/$(FRAMEWORKS_DIR_REL); \
	echo "Exec Path (shell): $$_SHELL_CURRENT_EXEC_PATH_"; \
	echo "Frameworks Path (shell): $$_SHELL_CURRENT_FW_PATH_"; \
	for _pattern_loop_var in $(DYLIB_PATTERNS_TO_BUNDLE); do \
		echo "Processing dylib pattern (shell loop): $$_pattern_loop_var"; \
		$(call process_dylib,$$_pattern_loop_var,$$_SHELL_CURRENT_EXEC_PATH_,$$_SHELL_CURRENT_FW_PATH_); \
	done; \
	echo "--- Dylib processing finished ---"

	# Copy ffmpeg to Resources
	$(call copy_ffmpeg)

	# Make ffmpeg executable
	chmod +x "./$(APP_BUNDLE_DIR)/$(RESOURCES_DIR_REL)/ffmpeg"

	# Сюда можно добавить копирование других ресурсов, например, иконки
	if [ -f "TapeXPlayer.icns" ]; then \
		echo "Copying TapeXPlayer.icns to Resources..."; \
		cp "TapeXPlayer.icns" "./$(APP_BUNDLE_DIR)/$(RESOURCES_DIR_REL)/TapeXPlayer.icns"; \
	else \
		echo "Warning: TapeXPlayer.icns not found in workspace root. Icon will not be bundled." >&2; \
	fi
	@echo "--- Bundle for $(TARGET_NAME) created in ./$(APP_BUNDLE_DIR) ---"

run: bundle
	@echo "Running $(APP_BUNDLE_DIR)..."
	@open . 
	@open "./$(APP_BUNDLE_DIR)" # Указываем путь явно


# Правила компиляции объектных файлов (остаются почти без изменений)
obj/arm64/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -arch arm64 $(ARM_CFLAGS) -I"$(SRC_DIR)" -I"$(SRC_DIR)/common" -I"$(SRC_DIR)/main" -I"$(SRC_DIR)/core" -c $< -o $@

obj/x86_64/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -arch x86_64 $(X86_CFLAGS) -I"$(SRC_DIR)" -I"$(SRC_DIR)/common" -I"$(SRC_DIR)/main" -I"$(SRC_DIR)/core" -c $< -o $@

obj/arm64/%.o: $(SRC_DIR)/%.mm
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -ObjC++ -arch arm64 $(ARM_CFLAGS) -I"$(SRC_DIR)" -I"$(SRC_DIR)/common" -I"$(SRC_DIR)/main" -I"$(SRC_DIR)/core" -c $< -o $@

obj/x86_64/%.o: $(SRC_DIR)/%.mm
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -ObjC++ -arch x86_64 $(X86_CFLAGS) -I"$(SRC_DIR)" -I"$(SRC_DIR)/common" -I"$(SRC_DIR)/main" -I"$(SRC_DIR)/core" -c $< -o $@

clean:
	@echo "Cleaning up..."
	@rm -f $(ARM_TARGET) $(X86_TARGET) $(UNIVERSAL_TARGET)
	@rm -rf obj "./$(APP_BUNDLE_DIR)"
	@echo "Cleanup complete."

export MACOSX_DEPLOYMENT_TARGET=14.3
export SDKROOT=$(shell xcrun --show-sdk-path)
