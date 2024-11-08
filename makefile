ARCH ?= $(shell uname -m)


CXX = clang++
CXXFLAGS = -std=c++17
LDFLAGS = -lSDL2 -lSDL2_ttf -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lgstapp-1.0 -lavcodec -lavformat -lavutil -lswscale -lswresample -pthread -lportaudio
FRAMEWORKS = -framework VideoToolbox -framework AudioToolbox -framework CoreFoundation -framework CoreVideo -framework CoreMedia -framework AppKit

# Settings for M1 (arm64)
ifeq ($(ARCH),arm64)
    HOMEBREW_PREFIX = /opt/homebrew
    CXXFLAGS += -I$(HOMEBREW_PREFIX)/include -I$(HOMEBREW_PREFIX)/include/gstreamer-1.0 -I$(HOMEBREW_PREFIX)/include/glib-2.0 -I$(HOMEBREW_PREFIX)/lib/glib-2.0/include -I$(HOMEBREW_PREFIX)/include/SDL2
    LDFLAGS += -L$(HOMEBREW_PREFIX)/lib -Wl,-rpath,$(HOMEBREW_PREFIX)/lib
else ifeq ($(ARCH),x86_64)
    # Settings for Intel (x86_64)
    HOMEBREW_PREFIX = /usr/local
    CXXFLAGS += -I$(HOMEBREW_PREFIX)/include -I$(HOMEBREW_PREFIX)/include/gstreamer-1.0 -I$(HOMEBREW_PREFIX)/include/glib-2.0 -I$(HOMEBREW_PREFIX)/lib/glib-2.0/include -I$(HOMEBREW_PREFIX)/include/SDL2
    LDFLAGS += -L$(HOMEBREW_PREFIX)/lib
else
    $(error Unsupported architecture: $(ARCH))
endif

# OpenSSL settings (common for both architectures)
CXXFLAGS += -I$(HOMEBREW_PREFIX)/opt/openssl/include
LDFLAGS += -L$(HOMEBREW_PREFIX)/opt/openssl/lib -lssl -lcrypto

CXXFLAGS += -framework UniformTypeIdentifiers
LDFLAGS += -framework UniformTypeIdentifiers

ifeq ($(USE_ALT_SOURCE),1)
    SRC_DIR = source_alt
else
    SRC_DIR = source
endif

SRCS = $(SRC_DIR)/mainau.cpp $(SRC_DIR)/main.cpp $(SRC_DIR)/decode.cpp $(SRC_DIR)/display.cpp
OBJS = $(patsubst $(SRC_DIR)/%.cpp,obj/%.o,$(SRCS))
TARGET = TapeXPlayer

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(FRAMEWORKS)

obj/%.o: $(SRC_DIR)/%.cpp | obj
	$(CXX) $(CXXFLAGS) -c $< -o $@

obj:
	mkdir -p obj

clean:
	rm -rf obj $(TARGET)

.PHONY: all clean

CXXFLAGS += -I"$(SRC_DIR)"
LDFLAGS += -L"$(SRC_DIR)" -lnfd

$(info NFD_LIB_PATH=$(SRC_DIR)/libnfd.a)

export MACOSX_DEPLOYMENT_TARGET=14.5
export SDKROOT=$(shell xcrun --show-sdk-path)
