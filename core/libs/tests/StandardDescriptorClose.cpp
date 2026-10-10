#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

extern "C" {
int APS5_VABI sceKernelClose(int);
int APS5_VABI close_nid_postfix(int);
int APS5_VABI _close_nid_postfix(int);
int* APS5_VABI __error_nid_postfix();
}

void Require(bool condition) {
    if (!condition) throw std::runtime_error("standard descriptor close check failed");
}

int main() {
    for (int fd = 0; fd < 3; ++fd) {
#ifdef _WIN32
        const auto original = _get_osfhandle(fd);
#else
        const auto original = fcntl(fd, F_GETFD);
#endif
        for (int attempt = 0; attempt < 2; ++attempt) {
            Require(sceKernelClose(fd) == SCE_KERNEL_ERROR_EPERM);
            *__error_nid_postfix() = 0;
            Require(close_nid_postfix(fd) == -1 && *__error_nid_postfix() == 1);
            *__error_nid_postfix() = 0;
            Require(_close_nid_postfix(fd) == -1 && *__error_nid_postfix() == 1);
        }
#ifdef _WIN32
        Require(_get_osfhandle(fd) == original);
#else
        Require(fcntl(fd, F_GETFD) == original);
#endif
    }
    auto* file = std::tmpfile();
    Require(file != nullptr);
    Require(std::fputs("still open", file) >= 0 && std::fflush(file) == 0);
#ifdef _WIN32
    const int duplicate = _dup(_fileno(file));
#else
    const int duplicate = dup(fileno(file));
#endif
    Require(duplicate >= 3 && sceKernelClose(duplicate) == 0);
    Require(sceKernelClose(0) == SCE_KERNEL_ERROR_EPERM);
    std::rewind(file);
    std::array<char, 16> content{};
    Require(std::fread(content.data(), 1, 10, file) == 10);
    Require(std::strcmp(content.data(), "still open") == 0);
    Require(std::fclose(file) == 0);
    Require(sceKernelClose(-1) == SCE_KERNEL_ERROR_EBADF);
    std::puts("standard descriptor close: PASS");
}
