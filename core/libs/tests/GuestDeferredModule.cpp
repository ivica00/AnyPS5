#include "prx/libc/include/general/VabiMacros.hpp"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" {
int APS5_VABI sceKernelLoadStartModule(const char*, std::size_t, const void*, std::uint32_t, const void*, int*);
int APS5_VABI sceKernelStopUnloadModule(int, std::size_t, const void*, std::uint32_t, const void*, int*);
int APS5_VABI sceKernelDlsym(int, const char*, void**);
}

static void Require(bool value) { if (!value) std::abort(); }

int main(int argc, char** argv) {
    Require(argc == 2);
    auto* image = reinterpret_cast<std::uint8_t*>(LoadLibraryA(argv[1]));
    Require(image != nullptr);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    std::uint32_t* ready = nullptr;
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        if (std::memcmp(sections[index].Name, ".aps5mod", 8) == 0) ready = reinterpret_cast<std::uint32_t*>(image + sections[index].VirtualAddress + 4);
    }
    Require(ready && *ready == 0);
    int result = -1;
    const int argument = 0x100;
    Require(sceKernelLoadStartModule(argv[1], 16, &argument, 0, nullptr, &result) < 0);
    *ready = 1;
    const auto first = sceKernelLoadStartModule(argv[1], 16, &argument, 0, nullptr, &result);
    Require(first > 0 && result == (16 ^ argument));
    void* symbol = nullptr;
    Require(sceKernelDlsym(first, "shared", &symbol) == 0);
    Require(reinterpret_cast<int (APS5_VABI *)()>(symbol)() == 42);
    const auto second = sceKernelLoadStartModule(argv[1], 77, &argument, 0, nullptr, &result);
    Require(second > 0 && result == (16 ^ argument));
    Require(sceKernelStopUnloadModule(first, 31, &argument, 0, nullptr, &result) == 0 && result == 0);
    Require(sceKernelStopUnloadModule(second, 33, &argument, 0, nullptr, &result) == 0 && result == (33 ^ argument));
    const auto third = sceKernelLoadStartModule(argv[1], 5, &argument, 0, nullptr, &result);
    Require(third > 0 && result == (5 ^ argument));
    Require(sceKernelStopUnloadModule(third, 9, &argument, 0, nullptr, &result) == 0 && result == (9 ^ argument));
    Require(FreeLibrary(reinterpret_cast<HMODULE>(image)) != 0);
}
