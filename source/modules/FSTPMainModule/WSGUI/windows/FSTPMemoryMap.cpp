#ifdef _WIN32
#include "FSTPMemoryMap.h"

void* fstp_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    HANDLE hFile = INVALID_HANDLE_VALUE;
    if (fd != -1) {
        hFile = (HANDLE)_get_osfhandle(fd);
    }

    DWORD flProtect = 0;
    if ((prot & PROT_READ) && !(prot & PROT_WRITE)) flProtect = PAGE_READONLY;
    if (prot & PROT_WRITE) flProtect = PAGE_READWRITE;
    if (prot & PROT_EXEC) flProtect = PAGE_EXECUTE_READ;

    HANDLE hMapping = CreateFileMapping(
        hFile,
        NULL,
        flProtect,
        (DWORD)((length >> 32) & 0xFFFFFFFF),
        (DWORD)(length & 0xFFFFFFFF),
        NULL
    );

    if (hMapping == NULL) {
        return MAP_FAILED;
    }

    DWORD dwDesiredAccess = 0;
    if (prot & PROT_READ) dwDesiredAccess |= FILE_MAP_READ;
    if (prot & PROT_WRITE) dwDesiredAccess |= FILE_MAP_WRITE;
    if (prot & PROT_EXEC) dwDesiredAccess |= FILE_MAP_EXECUTE;

    void* map = MapViewOfFile(
        hMapping,
        dwDesiredAccess,
        (DWORD)((offset >> 32) & 0xFFFFFFFF),
        (DWORD)(offset & 0xFFFFFFFF),
        length
    );

    CloseHandle(hMapping);

    if (map == NULL) {
        return MAP_FAILED;
    }

    return map;
}

int fstp_munmap(void* addr, size_t length) {
    (void)length; // Unused in Windows implementation
    return UnmapViewOfFile(addr) ? 0 : -1;
}
#else
#include <sys/mman.h>

// No-op stubs for non-Windows builds (should never be called).
void* fstp_mmap(void* /*addr*/, size_t /*length*/, int /*prot*/, int /*flags*/, int /*fd*/, off_t /*offset*/) {
    return MAP_FAILED;
}

int fstp_munmap(void* /*addr*/, size_t /*length*/) {
    return -1;
}
#endif
