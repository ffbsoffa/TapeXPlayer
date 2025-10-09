#ifdef __linux__

#include "../main.h"
#include <iostream>

// Stub for Linux/Wayland main loop - not yet implemented
int RunMainUILoop() {
    std::cerr << "Linux/Wayland UI main loop not yet implemented" << std::endl;
    return -1;
}

#endif // __linux__