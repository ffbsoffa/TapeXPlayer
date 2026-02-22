#ifndef FSTP_MEMORY_MAP_H
#define FSTP_MEMORY_MAP_H

#ifdef _WIN32
#include <windows.h>
#include <cstddef>
#include <sys/types.h>
#include <io.h>

// Windows implementation of mmap-like functionality
void* fstp_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int fstp_munmap(void* addr, size_t length);

// Protection flags compatibility
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define PROT_NONE  0x0

// Map flags compatibility
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

// Error return
#define MAP_FAILED    ((void *)-1)
#endif

#endif // FSTP_MEMORY_MAP_H
