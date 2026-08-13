#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../vfs/vfs.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 255)
        return 0; // limit size to prevent massive allocs in fuzzer

    char path[256];
    memcpy(path, data, size);
    path[size] = '\0';

    // Fuzz the VFS open logic (which parses the path string)
    // Even if it fails to find the file, we want to ensure no buffer overflows occur.
    int fd = VfsManager::instance().open(path);
    if (fd >= 0) {
        VfsManager::instance().close(fd);
    }

    return 0;
}
