#ifndef CORE_LIBS_PRX_LIBKERNEL_FILE_FILELOCK_HPP
#define CORE_LIBS_PRX_LIBKERNEL_FILE_FILELOCK_HPP

#include <mutex>

namespace File {

int Flock(int fd, int operation);
void ForgetFileLock(int fd);
#ifdef _WIN32
std::mutex& DescriptorMutex(int fd);
#endif

}

#endif
