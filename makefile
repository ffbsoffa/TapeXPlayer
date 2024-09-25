CXX = clang++
CXXFLAGS = -std=c++17 -I/opt/homebrew/include -I/opt/homebrew/include/gstreamer-1.0 -I/opt/homebrew/include/glib-2.0 -I/opt/homebrew/lib/glib-2.0/include -I/opt/homebrew/include/SDL2 -I/usr/local/opt/openssl/include
LDFLAGS = -L/opt/homebrew/lib -lSDL2 -lSDL2_ttf -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lgstapp-1.0 -lavcodec -lavformat -lavutil -lswscale -lswresample -pthread -Wl,-rpath,/opt/homebrew/lib -L/usr/local/opt/openssl/lib -lssl -lcrypto
FRAMEWORKS = -framework VideoToolbox -framework AudioToolbox -framework CoreFoundation -framework CoreVideo -framework CoreMedia

ifeq ($(USE_ALT_SOURCE),1)
    SRC_DIR = source_alt
else
    SRC_DIR = source
endif

SRCS = $(SRC_DIR)/main.cpp $(SRC_DIR)/mainau.cpp
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