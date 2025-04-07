CXX = clang++
CXXFLAGS = -std=c++17

# Use alternative source by default
USE_ALT_SOURCE = 1
USE_DEBUG_SOURCE = 0

# Common flags and frameworks
FRAMEWORKS = -framework VideoToolbox -framework AudioToolbox -framework CoreFoundation \
            -framework CoreVideo -framework CoreMedia -framework AppKit -framework CoreMIDI \
            -framework CoreAudio -framework UniformTypeIdentifiers -framework OpenGL \
            -framework Cocoa

# Common libraries
LIBS = -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lSDL2 -lSDL2_ttf -lrtmidi \
       -lportaudio -lavcodec -lavformat -lavutil -lswscale -lswresample \
       -lssl -lcrypto -lGLEW

# ARM64 settings
ARM_PREFIX = /opt/homebrew
ARM_CFLAGS = -I$(ARM_PREFIX)/include \
             -I$(ARM_PREFIX)/include/gstreamer-1.0 \
             -I$(ARM_PREFIX)/include/glib-2.0 \
             -I$(ARM_PREFIX)/lib/glib-2.0/include \
             -I$(ARM_PREFIX)/include/SDL2 \
             -I$(ARM_PREFIX)/Cellar/rtmidi/6.0.0/include/rtmidi \
             -I$(ARM_PREFIX)/opt/openssl/include \
             -I$(SRC_DIR)/nfd/include

ARM_LDFLAGS = -L$(ARM_PREFIX)/lib \
              -L$(ARM_PREFIX)/opt/openssl/lib \
              -L$(ARM_PREFIX)/Cellar/rtmidi/6.0.0/lib \
              -L$(ARM_PREFIX)/Cellar/ffmpeg/7.1_4/lib \
              -L$(ARM_PREFIX)/Cellar/sdl2/2.30.11/lib \
              -L$(ARM_PREFIX)/Cellar/sdl2_ttf/2.24.0/lib \
              -L$(ARM_PREFIX)/Cellar/portaudio/19.7.0/lib \
              -Wl,-rpath,$(ARM_PREFIX)/lib \
              -Wl,-rpath,$(ARM_PREFIX)/opt/openssl/lib \
              -Wl,-rpath,$(ARM_PREFIX)/Cellar/rtmidi/6.0.0/lib \
              -Wl,-rpath,$(ARM_PREFIX)/Cellar/ffmpeg/7.1_4/lib \
              -Wl,-rpath,$(ARM_PREFIX)/Cellar/sdl2/2.30.11/lib \
              -Wl,-rpath,$(ARM_PREFIX)/Cellar/sdl2_ttf/2.24.0/lib \
              -Wl,-rpath,$(ARM_PREFIX)/Cellar/portaudio/19.7.0/lib

# x86_64 settings
X86_PREFIX = /usr/local
X86_CFLAGS = -I$(X86_PREFIX)/include \
             -I$(X86_PREFIX)/include/gstreamer-1.0 \
             -I$(X86_PREFIX)/include/glib-2.0 \
             -I$(X86_PREFIX)/lib/glib-2.0/include \
             -I$(X86_PREFIX)/include/SDL2 \
             -I$(X86_PREFIX)/Cellar/rtmidi/6.0.0/include/rtmidi \
             -I$(X86_PREFIX)/opt/openssl/include \
             -I$(SRC_DIR)/nfd/include

X86_LDFLAGS = -L$(X86_PREFIX)/lib \
              -L$(X86_PREFIX)/opt/openssl/lib \
              -L$(X86_PREFIX)/Cellar/rtmidi/6.0.0/lib \
              -L$(X86_PREFIX)/Cellar/ffmpeg/7.1_4/lib \
              -L$(X86_PREFIX)/Cellar/sdl2/2.30.11/lib \
              -L$(X86_PREFIX)/Cellar/sdl2_ttf/2.24.0/lib \
              -L$(X86_PREFIX)/Cellar/portaudio/19.7.0/lib \
              -Wl,-rpath,$(X86_PREFIX)/lib \
              -Wl,-rpath,$(X86_PREFIX)/opt/openssl/lib \
              -Wl,-rpath,$(X86_PREFIX)/Cellar/rtmidi/6.0.0/lib \
              -Wl,-rpath,$(X86_PREFIX)/Cellar/ffmpeg/7.1_4/lib \
              -Wl,-rpath,$(X86_PREFIX)/Cellar/sdl2/2.30.11/lib \
              -Wl,-rpath,$(X86_PREFIX)/Cellar/sdl2_ttf/2.24.0/lib \
              -Wl,-rpath,$(X86_PREFIX)/Cellar/portaudio/19.7.0/lib

ifeq ($(USE_DEBUG_SOURCE),1)
    ARM_NFD_LIB_PATH=source_debug/libnfd.a
    X86_NFD_LIB_PATH=source_debug/libnfd_x86_64.a
    SRC_DIR=source_debug
else ifeq ($(USE_ALT_SOURCE),1)
    ARM_NFD_LIB_PATH=source_alt/libnfd.a
    X86_NFD_LIB_PATH=source_alt/libnfd_x86_64.a
    SRC_DIR=source_alt
else
    ARM_NFD_LIB_PATH=source/libnfd.a
    X86_NFD_LIB_PATH=source/libnfd_x86_64.a
    SRC_DIR=source
endif

SRCS = $(wildcard $(SRC_DIR)/*.cpp)
OBJC_SRCS = $(wildcard $(SRC_DIR)/*.mm)
ARM_OBJS = $(SRCS:$(SRC_DIR)/%.cpp=obj/arm64/%.o) $(OBJC_SRCS:$(SRC_DIR)/%.mm=obj/arm64/%.o)
X86_OBJS = $(SRCS:$(SRC_DIR)/%.cpp=obj/x86_64/%.o) $(OBJC_SRCS:$(SRC_DIR)/%.mm=obj/x86_64/%.o)

TARGET = TapeXPlayer
ifeq ($(USE_DEBUG_SOURCE),1)
    TARGET = TapeXDebugger
endif
ARM_TARGET = $(TARGET)_arm64
X86_TARGET = $(TARGET)_x86_64

.PHONY: all clean arm64 x86_64 debug

all: universal

debug:
	$(MAKE) USE_DEBUG_SOURCE=1

universal: arm64 x86_64
	lipo -create -output $(TARGET) $(ARM_TARGET) $(X86_TARGET)

arm64: $(ARM_TARGET)

x86_64: $(X86_TARGET)

$(ARM_TARGET): $(ARM_OBJS)
	$(CXX) -arch arm64 $(ARM_OBJS) $(ARM_NFD_LIB_PATH) $(ARM_LDFLAGS) $(LIBS) $(FRAMEWORKS) -o $@

$(X86_TARGET): $(X86_OBJS)
	$(CXX) -arch x86_64 $(X86_OBJS) $(X86_NFD_LIB_PATH) $(X86_LDFLAGS) $(LIBS) $(FRAMEWORKS) -o $@

obj/arm64/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p obj/arm64
	$(CXX) $(CXXFLAGS) -arch arm64 $(ARM_CFLAGS) -I"$(SRC_DIR)" -c $< -o $@

obj/x86_64/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p obj/x86_64
	$(CXX) $(CXXFLAGS) -arch x86_64 $(X86_CFLAGS) -I"$(SRC_DIR)" -c $< -o $@

obj/arm64/%.o: $(SRC_DIR)/%.mm
	@mkdir -p obj/arm64
	$(CXX) $(CXXFLAGS) -arch arm64 $(ARM_CFLAGS) -I"$(SRC_DIR)" -c $< -o $@

obj/x86_64/%.o: $(SRC_DIR)/%.mm
	@mkdir -p obj/x86_64
	$(CXX) $(CXXFLAGS) -arch x86_64 $(X86_CFLAGS) -I"$(SRC_DIR)" -c $< -o $@

clean:
	rm -rf obj $(TARGET) $(ARM_TARGET) $(X86_TARGET)

export MACOSX_DEPLOYMENT_TARGET=14.5
export SDKROOT=$(shell xcrun --show-sdk-path)
