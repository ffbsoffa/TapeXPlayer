#pragma once

// Names the calling thread so per-thread CPU shows up by role in top -H / htop / Instruments
// (otherwise every thread is just "TapeXPlayer_lin"). Linux caps names at 15 characters.
// Windows: no-op — SetThreadDescription would need runtime loading to keep Windows 7 starting.

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

inline void FSTPSetThreadName(const char* name) {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    (void)name;
#endif
}
