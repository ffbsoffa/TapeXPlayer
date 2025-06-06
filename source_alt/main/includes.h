#ifndef INCLUDES_H
#define INCLUDES_H

// Standard C++ headers
#include <iostream>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <future>
#include <cstdlib>
#include <cstring> // For strerror
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <limits.h>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm> // For std::transform

// System headers
#include <sys/resource.h>

// SDL headers
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

// Third-party headers
#include "nfd.hpp"

// Project core headers - decode
#include "core/decode/decode.h"
#include "core/decode/low_cached_decoder_manager.h"
#include "core/decode/full_res_decoder_manager.h"
#include "core/decode/low_res_decoder.h"
#include "core/decode/cached_decoder.h"
#include "../core/decode/cached_decoder_manager.h"

// Project core headers - display
#include "core/display/display.h"
#include "core/display/screenshot.h"
#include "core/display/window_manager.h"

// Project core headers - remote
#include "core/remote/remote_control.h"
#include "core/remote/url_handler.h"

// Project core headers - menu
#include "core/menu/menu_system.h"

// Project common headers
#include "common/common.h"
#include "common/fontdata.h"

// Project main headers
#include "deep_pause_manager.h"
#include "keyboard_manager.h"
#include "main.h"
#include "initmanager.h"
#include "globals.h"

#endif // INCLUDES_H