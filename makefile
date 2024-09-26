# Определение архитектуры
ARCH := $(shell uname -m)

# Общие настройки
CXX = clang++
CXXFLAGS = -std=c++17
LDFLAGS = -lSDL2 -lSDL2_ttf -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lgstapp-1.0 -lavcodec -lavformat -lavutil -lswscale -lswresample -pthread
FRAMEWORKS = -framework VideoToolbox -framework AudioToolbox -framework CoreFoundation -framework CoreVideo -framework CoreMedia

# Настройки для M1 (arm64)
ifeq ($(ARCH),arm64)
    HOMEBREW_PREFIX = /opt/homebrew
    CXXFLAGS += -I$(HOMEBREW_PREFIX)/include -I$(HOMEBREW_PREFIX)/include/gstreamer-1.0 -I$(HOMEBREW_PREFIX)/include/glib-2.0 -I$(HOMEBREW_PREFIX)/lib/glib-2.0/include -I$(HOMEBREW_PREFIX)/include/SDL2
    LDFLAGS += -L$(HOMEBREW_PREFIX)/lib -Wl,-rpath,$(HOMEBREW_PREFIX)/lib
else
    # Настройки для Intel (x86_64)
    HOMEBREW_PREFIX = /usr/local
    CXXFLAGS += -I$(HOMEBREW_PREFIX)/include -I$(HOMEBREW_PREFIX)/include/gstreamer-1.0 -I$(HOMEBREW_PREFIX)/include/glib-2.0 -I$(HOMEBREW_PREFIX)/lib/glib-2.0/include -I$(HOMEBREW_PREFIX)/include/SDL2
    LDFLAGS += -L$(HOMEBREW_PREFIX)/lib
endif

# OpenSSL настройки (общие для обеих архитектур)
CXXFLAGS += -I$(HOMEBREW_PREFIX)/opt/openssl/include
LDFLAGS += -L$(HOMEBREW_PREFIX)/opt/openssl/lib -lssl -lcrypto

ifeq ($(USE_ALT_SOURCE),1)
    SRC_DIR = source_alt
else
    SRC_DIR = source
endif

SRCS = $(SRC_DIR)/mainau.cpp $(SRC_DIR)/framedebugger.cpp $(SRC_DIR)/decode.cpp
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