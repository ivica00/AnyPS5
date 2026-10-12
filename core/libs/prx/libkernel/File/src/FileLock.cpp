#include "prx/libkernel/File/include/FileLock.hpp"
#include "prx/libc/include/FileLock.hpp"

#ifdef _WIN32

namespace File {

static std::mutex g_descriptorMutexes[64];

std::mutex& DescriptorMutex(int fd) {
    return g_descriptorMutexes[static_cast<unsigned int>(fd >= 0 ? fd : 0) % 64];
}

int Flock(int fd, int operation) {
    return GuestFileLock_nid_no_patch(fd, operation);
}

void ForgetFileLock(int fd) {
    GuestFileLockForget_nid_no_patch(fd);
}

}

#endif
